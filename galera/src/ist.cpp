//
// Copyright (C) 2011-2014 Codership Oy <info@codership.com>
//

#include "ist.hpp"
#include "ist_proto.hpp"

#include "gu_logger.hpp"
#include "gu_uri.hpp"

#include "galera_common.hpp"
#include <boost/bind.hpp>
#include <fstream>
#include <algorithm>

namespace
{
    static std::string const CONF_KEEP_KEYS     ("ist.keep_keys");
    static bool        const CONF_KEEP_KEYS_DEFAULT (true);
}


namespace galera
{
    namespace ist
    {
        class AsyncSender : public Sender
        {
        public:
            AsyncSender(const gu::Config& conf,
                        const std::string& peer,
                        wsrep_seqno_t first,
                        wsrep_seqno_t last,
                        wsrep_seqno_t rebuild_start,
                        AsyncSenderMap& asmap,
                        int version)
                :
                Sender (conf, asmap.gcache(), peer, version),
                conf_  (conf),
                peer_  (peer),
                first_ (first),
                last_  (last),
                rebuild_start_(rebuild_start),
                asmap_ (asmap),
                thread_()
            { }

            const gu::Config&  conf()   { return conf_;   }
            const std::string& peer()   { return peer_;   }
            wsrep_seqno_t      first()  { return first_;  }
            wsrep_seqno_t      last()   { return last_;   }
            wsrep_seqno_t      rebuild_start() { return rebuild_start_; }
            AsyncSenderMap&    asmap()  { return asmap_;  }
            pthread_t          thread() { return thread_; }

        private:

            friend class AsyncSenderMap;
            const gu::Config&  conf_;
            const std::string  peer_;
            wsrep_seqno_t      first_;
            wsrep_seqno_t      last_;
            wsrep_seqno_t      rebuild_start_;
            AsyncSenderMap&    asmap_;
            pthread_t          thread_;
        };
    }
}


std::string const
galera::ist::Receiver::RECV_ADDR("ist.recv_addr");

void
galera::ist::register_params(gu::Config& conf)
{
    conf.add(Receiver::RECV_ADDR);
    conf.add(CONF_KEEP_KEYS);
}

galera::ist::Receiver::Receiver(gu::Config&           conf,
                                TrxHandleSlave::Pool& sp,
                                gcache::GCache&       gc,
                                PreISTHandler&        pre_ist,
                                const char*           addr)
    :
    io_service_   (),
    acceptor_     (io_service_),
    ssl_ctx_      (io_service_, asio::ssl::context::sslv23),
    mutex_        (),
    cond_         (),
    consumers_    (),
    first_seqno_  (-1),
    current_seqno_(-1),
    last_seqno_   (-1),
    conf_         (conf),
    trx_pool_     (sp),
    gcache_       (gc),
    thread_       (),
    error_code_   (0),
    version_      (-1),
    use_ssl_      (false),
    running_      (false),
    ready_        (false),
    pre_ist_      (pre_ist)
{
    std::string recv_addr;

    try /* check if receive address is explicitly set */
    {
        recv_addr = conf_.get(RECV_ADDR);
        return;
    }
    catch (gu::NotSet& e) {} /* if not, check the alternative.
                                TODO: try to find from system. */

    if (addr)
    {
        try
        {
            recv_addr = gu::URI(std::string("tcp://") + addr).get_host();
            conf_.set(RECV_ADDR, recv_addr);
        }
        catch (gu::NotSet& e) {}
    }
}


galera::ist::Receiver::~Receiver()
{ }


extern "C" void* run_receiver_thread(void* arg)
{
    galera::ist::Receiver* receiver(static_cast<galera::ist::Receiver*>(arg));
    receiver->run();
    return 0;
}

static std::string
IST_determine_recv_addr (gu::Config& conf)
{
    std::string recv_addr;

    try
    {
        recv_addr = conf.get(galera::ist::Receiver::RECV_ADDR);
    }
    catch (gu::NotFound&)
    {
        try
        {
            recv_addr = conf.get(galera::BASE_HOST_KEY);
        }
        catch (gu::NotSet&)
        {
            gu_throw_error(EINVAL)
                << "Could not determine IST receinve address: '"
                << galera::ist::Receiver::RECV_ADDR << "' not set.";
        }
    }

    /* check if explicit scheme is present */
    if (recv_addr.find("://") == std::string::npos)
    {
        bool ssl(false);

        try
        {
            std::string ssl_key = conf.get(gu::conf::ssl_key);
            if (ssl_key.length() != 0) ssl = true;
        }
        catch (gu::NotSet&) {}

        if (ssl)
            recv_addr.insert(0, "ssl://");
        else
            recv_addr.insert(0, "tcp://");
    }

    gu::URI ra_uri(recv_addr);

    if (!conf.has(galera::BASE_HOST_KEY))
        conf.set(galera::BASE_HOST_KEY, ra_uri.get_host());

    try /* check for explicit port,
           TODO: make it possible to use any free port (explicit 0?) */
    {
        ra_uri.get_port();
    }
    catch (gu::NotSet&) /* use gmcast listen port + 1 */
    {
        int port(0);

        try
        {
            port = gu::from_string<uint16_t>(
//                 gu::URI(conf.get("gmcast.listen_addr")).get_port()
                    conf.get(galera::BASE_PORT_KEY)
                );

        }
        catch (...)
        {
            port = gu::from_string<uint16_t>(galera::BASE_PORT_DEFAULT);
        }

        port += 1;

        recv_addr += ":" + gu::to_string(port);
    }

    return recv_addr;
}

std::string
galera::ist::Receiver::prepare(wsrep_seqno_t first_seqno,
                               wsrep_seqno_t last_seqno,
                               int           version)
{
    ready_ = false;
    version_ = version;
    recv_addr_ = IST_determine_recv_addr(conf_);
    gu::URI     const uri(recv_addr_);
    try
    {
        if (uri.get_scheme() == "ssl")
        {
            log_info << "IST receiver using ssl";
            use_ssl_ = true;
            // Protocol versions prior 7 had a bug on sender side
            // which made sender to return null cert in handshake.
            // Therefore peer cert verfification must be enabled
            // only at protocol version 7 or higher.
            gu::ssl_prepare_context(conf_, ssl_ctx_, version >= 7);
        }

        asio::ip::tcp::resolver resolver(io_service_);
        asio::ip::tcp::resolver::query
            query(gu::unescape_addr(uri.get_host()),
                  uri.get_port(),
                  asio::ip::tcp::resolver::query::flags(0));
        asio::ip::tcp::resolver::iterator i(resolver.resolve(query));
        acceptor_.open(i->endpoint().protocol());
        acceptor_.set_option(asio::ip::tcp::socket::reuse_address(true));
        gu::set_fd_options(acceptor_);
        acceptor_.bind(*i);
        acceptor_.listen();
        // read recv_addr_ from acceptor_ in case zero port was specified
        recv_addr_ = uri.get_scheme()
            + "://"
            + uri.get_host()
            + ":"
            + gu::to_string(acceptor_.local_endpoint().port());
    }
    catch (asio::system_error& e)
    {
        recv_addr_ = "";
        gu_throw_error(e.code().value())
            << "Failed to open IST listener at "
            << uri.to_string()
            << "', asio error '" << e.what() << "'";
    }

    first_seqno_   = first_seqno;
    current_seqno_ = -1;
    last_seqno_    = last_seqno;
    int err;
    if ((err = pthread_create(&thread_, 0, &run_receiver_thread, this)) != 0)
    {
        recv_addr_ = "";
        gu_throw_error(err) << "Unable to create receiver thread";
    }

    running_ = true;

    log_info << "Prepared IST receiver, listening at: "
             << (uri.get_scheme()
                 + "://"
                 + gu::escape_addr(acceptor_.local_endpoint().address())
                 + ":"
                 + gu::to_string(acceptor_.local_endpoint().port()));
    return recv_addr_;
}


void galera::ist::Receiver::run()
{
    asio::ip::tcp::socket socket(io_service_);
    asio::ssl::stream<asio::ip::tcp::socket> ssl_stream(io_service_, ssl_ctx_);
    try
    {
        if (use_ssl_ == true)
        {
            acceptor_.accept(ssl_stream.lowest_layer());
            gu::set_fd_options(ssl_stream.lowest_layer());
            ssl_stream.handshake(asio::ssl::stream<asio::ip::tcp::socket>::server);
        }
        else
        {
            acceptor_.accept(socket);
            gu::set_fd_options(socket);
        }
    }
    catch (asio::system_error& e)
    {
        gu_throw_error(e.code().value()) << "accept() failed"
                                         << "', asio error '"
                                         << e.what() << "': "
                                         << gu::extra_error_info(e.code());
    }
    acceptor_.close();
    int ec(0);
    try
    {
        bool const keep_keys(conf_.get(CONF_KEEP_KEYS, CONF_KEEP_KEYS_DEFAULT));
        Proto p(trx_pool_, gcache_, version_, keep_keys);

        if (use_ssl_ == true)
        {
            p.send_handshake(ssl_stream);
            p.recv_handshake_response(ssl_stream);
            p.send_ctrl(ssl_stream, Ctrl::C_OK);
        }
        else
        {
            p.send_handshake(socket);
            p.recv_handshake_response(socket);
            p.send_ctrl(socket, Ctrl::C_OK);
        }
        while (true)
        {
            gu::Lock lock(mutex_);
            while (ready_ == false || consumers_.empty())
            {
                lock.wait(cond_);
            }

            TrxHandleSlave* trx;
            std::pair<TrxHandleSlave*, bool> ret;
            if (use_ssl_ == true)
            {
                ret = p.recv_trx(ssl_stream);
            }
            else
            {
                ret = p.recv_trx(socket);
            }
            trx = ret.first;

            // Verify that the sequence of trx is continuous
            if (trx != 0)
            {
                if (current_seqno_ == -1)
                {
                    current_seqno_ = trx->global_seqno();
                }
                else if (trx->global_seqno() != current_seqno_)
                {
                    log_error << "unexpected trx seqno: " << trx->global_seqno()
                              << " expected: " << current_seqno_;
                    ec = EINVAL;
                    goto err;
                }
            }

            if (ret.second == true)
            {
                // Trx was received with index rebuild flag on
                pre_ist_.pre_ist_handle_trx(trx);
            }
            if (trx != 0 && first_seqno_ > 0 && current_seqno_ >= first_seqno_)
            {
                gu::Lock lock(mutex_);
                while (ready_ == false || consumers_.empty())
                {
                    lock.wait(cond_);
                }
                Consumer* cons(consumers_.top());
                consumers_.pop();
                cons->trx(trx);
                cons->cond().signal();
            }
            else if (trx != 0)
            {
                trx->unref();
            }

            ++current_seqno_;

            if (trx == 0)
            {
                log_debug << "eof received, closing socket";
                break;
            }
        }
    }
    catch (asio::system_error& e)
    {
        log_error << "got error while reading ist stream: " << e.code();
        ec = e.code().value();
    }
    catch (gu::Exception& e)
    {
        ec = e.get_errno();
        if (ec != EINTR)
        {
            log_error << "got exception while reading ist stream: " << e.what();
        }
    }

err:
    gu::Lock lock(mutex_);
    if (use_ssl_ == true)
    {
        ssl_stream.lowest_layer().close();
        // ssl_stream.shutdown();
    }
    else
    {
        socket.close();
    }

    running_ = false;
    if (ec != EINTR && current_seqno_ - 1 < last_seqno_)
    {
        log_error << "IST didn't contain all write sets, expected last: "
                  << last_seqno_ << " last received: " << current_seqno_ - 1;
        ec = EPROTO;
    }
    if (ec != EINTR)
    {
        error_code_ = ec;
    }
    while (consumers_.empty() == false)
    {
        consumers_.top()->cond().signal();
        consumers_.pop();
    }
}


void galera::ist::Receiver::ready()
{
    gu::Lock lock(mutex_);
    ready_ = true;
    cond_.signal();
}

int galera::ist::Receiver::recv(TrxHandleSlave** trx)
{
    Consumer cons;
    gu::Lock lock(mutex_);
    if (running_ == false)
    {
        if (error_code_ != 0)
        {
            gu_throw_error(error_code_) << "IST receiver reported error";
        }
        return EINTR;
    }
    consumers_.push(&cons);
    cond_.signal();
    lock.wait(cons.cond());
    if (cons.trx() == 0)
    {
        if (error_code_ != 0)
        {
            gu_throw_error(error_code_) << "IST receiver reported error";
        }
        return EINTR;
    }
    *trx = cons.trx();
    return 0;
}


wsrep_seqno_t galera::ist::Receiver::finished()
{
    if (recv_addr_ == "")
    {
        log_debug << "IST was not prepared before calling finished()";
    }
    else
    {
        interrupt();

        int err;
        if ((err = pthread_join(thread_, 0)) != 0)
        {
            log_warn << "Failed to join IST receiver thread: " << err;
        }

        acceptor_.close();

        gu::Lock lock(mutex_);

        running_ = false;

        while (consumers_.empty() == false)
        {
            consumers_.top()->cond().signal();
            consumers_.pop();
        }

        recv_addr_ = "";
    }

    return (current_seqno_ - 1);
}


void galera::ist::Receiver::interrupt()
{
    gu::URI uri(recv_addr_);
    try
    {
        asio::ip::tcp::resolver::iterator i;
        try
        {
            asio::ip::tcp::resolver resolver(io_service_);
            asio::ip::tcp::resolver::query
                query(gu::unescape_addr(uri.get_host()),
                      uri.get_port(),
                      asio::ip::tcp::resolver::query::flags(0));
            i = resolver.resolve(query);
        }
        catch (asio::system_error& e)
        {
            gu_throw_error(e.code().value())
                << "failed to resolve host '"
                << uri.to_string()
                << "', asio error '" << e.what() << "'";
        }
        if (use_ssl_ == true)
        {
            asio::ssl::stream<asio::ip::tcp::socket>
                ssl_stream(io_service_, ssl_ctx_);
            ssl_stream.lowest_layer().connect(*i);
            gu::set_fd_options(ssl_stream.lowest_layer());
            ssl_stream.handshake(asio::ssl::stream<asio::ip::tcp::socket>::client);
            Proto p(trx_pool_,
                    gcache_,
                    version_, conf_.get(CONF_KEEP_KEYS, CONF_KEEP_KEYS_DEFAULT));
            p.recv_handshake(ssl_stream);
            p.send_ctrl(ssl_stream, Ctrl::C_EOF);
            p.recv_ctrl(ssl_stream);
        }
        else
        {
            asio::ip::tcp::socket socket(io_service_);
            socket.connect(*i);
            gu::set_fd_options(socket);
            Proto p(trx_pool_, gcache_, version_,
                    conf_.get(CONF_KEEP_KEYS, CONF_KEEP_KEYS_DEFAULT));
            p.recv_handshake(socket);
            p.send_ctrl(socket, Ctrl::C_EOF);
            p.recv_ctrl(socket);
        }
    }
    catch (asio::system_error& e)
    {
        // ignore
    }
}


galera::ist::Sender::Sender(const gu::Config&  conf,
                            gcache::GCache&    gcache,
                            const std::string& peer,
                            int                version)
    :
    io_service_(),
    socket_    (io_service_),
    ssl_ctx_   (io_service_, asio::ssl::context::sslv23),
    ssl_stream_(0),
    conf_      (conf),
    gcache_    (gcache),
    version_   (version),
    use_ssl_   (false)
{
    gu::URI uri(peer);
    try
    {
        asio::ip::tcp::resolver resolver(io_service_);
        asio::ip::tcp::resolver::query
            query(gu::unescape_addr(uri.get_host()),
                  uri.get_port(),
                  asio::ip::tcp::resolver::query::flags(0));
        asio::ip::tcp::resolver::iterator i(resolver.resolve(query));
        if (uri.get_scheme() == "ssl")
        {
            use_ssl_ = true;
        }
        if (use_ssl_ == true)
        {
            log_info << "IST sender using ssl";
            ssl_prepare_context(conf, ssl_ctx_);
            // ssl_stream must be created after ssl_ctx_ is prepared...
            ssl_stream_ = new asio::ssl::stream<asio::ip::tcp::socket>(
                io_service_, ssl_ctx_);
            ssl_stream_->lowest_layer().connect(*i);
            gu::set_fd_options(ssl_stream_->lowest_layer());
            ssl_stream_->handshake(asio::ssl::stream<asio::ip::tcp::socket>::client);
        }
        else
        {
            socket_.connect(*i);
            gu::set_fd_options(socket_);
        }
    }
    catch (asio::system_error& e)
    {
        gu_throw_error(e.code().value()) << "IST sender, failed to connect '"
                                         << peer.c_str() << "': " << e.what();
    }
}


galera::ist::Sender::~Sender()
{
    if (use_ssl_ == true)
    {
        ssl_stream_->lowest_layer().close();
        delete ssl_stream_;
    }
    else
    {
        socket_.close();
    }
    gcache_.seqno_unlock();
}

template <class S>
void send_eof(galera::ist::Proto& p, S& stream)
{

    p.send_ctrl(stream, galera::ist::Ctrl::C_EOF);

    // wait until receiver closes the connection
    try
    {
        gu::byte_t b;
        size_t n;
        n = asio::read(stream, asio::buffer(&b, 1));
        if (n > 0)
        {
            log_warn << "received " << n
                     << " bytes, expected none";
        }
    }
    catch (asio::system_error& e)
    { }
}

void galera::ist::Sender::send(wsrep_seqno_t first, wsrep_seqno_t last,
                               wsrep_seqno_t rebuild_start)
{
    if (first > last)
    {
        if (version_ < 8)
        {
            gu_throw_error(EINVAL) << "sender send first greater than last: "
                                   << first << " > " << last ;
        }
    }

    try
    {
        TrxHandleSlave::Pool unused(1, 0, "");
        Proto p(unused,
                gcache_,
                version_, conf_.get(CONF_KEEP_KEYS, CONF_KEEP_KEYS_DEFAULT));
        int32_t ctrl;

        if (use_ssl_ == true)
        {
            p.recv_handshake(*ssl_stream_);
            p.send_handshake_response(*ssl_stream_);
            ctrl = p.recv_ctrl(*ssl_stream_);
        }
        else
        {
            p.recv_handshake(socket_);
            p.send_handshake_response(socket_);
            ctrl = p.recv_ctrl(socket_);
        }
        if (ctrl < 0)
        {
            gu_throw_error(EPROTO)
                << "ist send failed, peer reported error: " << ctrl;
        }

        log_info << "IST sender " << first << " -> " << last;

        if (first > last)
        {
            if (use_ssl_ == true)
            {
                send_eof(p, *ssl_stream_);
            }
            else
            {
                send_eof(p, socket_);
            }
            return;
        }


        std::vector<gcache::GCache::Buffer> buf_vec(
            std::min(static_cast<size_t>(last - first + 1),
                     static_cast<size_t>(1024)));
        ssize_t n_read;
        while ((n_read = gcache_.seqno_get_buffers(buf_vec, first)) > 0)
        {
            //log_info << "read " << first << " + " << n_read << " from gcache";
            for (wsrep_seqno_t i(0); i < n_read; ++i)
            {
                // Rebuild start is the seqno of the lowest trx in
                // cert index at CC. If the cert index was completely
                // reset, rebuild_start will be zero and no rebuild flag
                // should be set.
                bool rebuild_flag(rebuild_start > 0 &&
                                  buf_vec[i].seqno_g() >= rebuild_start);
                if (use_ssl_ == true)
                {
                    p.send_trx(*ssl_stream_, buf_vec[i], rebuild_flag);
                }
                else
                {
                    p.send_trx(socket_, buf_vec[i], rebuild_flag);
                }

                if (buf_vec[i].seqno_g() == last)
                {
                    if (use_ssl_ == true)
                    {
                        send_eof(p, *ssl_stream_);
                    }
                    else
                    {
                        send_eof(p, socket_);
                    }
                    return;
                }
            }
            first += n_read;
            // resize buf_vec to avoid scanning gcache past last
            size_t next_size(std::min(static_cast<size_t>(last - first + 1),
                                      static_cast<size_t>(1024)));

            if (buf_vec.size() != next_size)
            {
                buf_vec.resize(next_size);
            }
        }
    }
    catch (asio::system_error& e)
    {
        gu_throw_error(e.code().value()) << "ist send failed: " << e.code()
                                         << "', asio error '" << e.what()
                                         << "'";
    }
}




extern "C"
void* run_async_sender(void* arg)
{
    galera::ist::AsyncSender* as(reinterpret_cast<galera::ist::AsyncSender*>(arg));
    log_info << "async IST sender starting to serve " << as->peer().c_str()
             << " sending " << as->first() << "-" << as->last();
    wsrep_seqno_t join_seqno;
    try
    {
        as->send(as->first(), as->last(), as->rebuild_start());
        join_seqno = as->last();
    }
    catch (gu::Exception& e)
    {
        log_error << "async IST sender failed to serve " << as->peer().c_str()
                  << ": " << e.what();
        join_seqno = -e.get_errno();
    }
    catch (...)
    {
        log_error << "async IST sender, failed to serve " << as->peer().c_str();
        throw;
    }

    try
    {
        as->asmap().remove(as, join_seqno);
        pthread_detach(as->thread());
        delete as;
    }
    catch (gu::NotFound& nf)
    {
        log_debug << "async IST sender already removed";
    }
    log_info << "async IST sender served";

    return 0;
}


void galera::ist::AsyncSenderMap::run(const gu::Config&  conf,
                                      const std::string& peer,
                                      wsrep_seqno_t      first,
                                      wsrep_seqno_t      last,
                                      wsrep_seqno_t      rebuild_start,
                                      int                version)
{
    gu::Critical crit(monitor_);
    AsyncSender* as(new AsyncSender(conf, peer, first, last, rebuild_start,
                                    *this, version));
    int err(pthread_create(&as->thread_, 0, &run_async_sender, as));
    if (err != 0)
    {
        delete as;
        gu_throw_error(err) << "failed to start sender thread";
    }
    senders_.insert(as);
}


void galera::ist::AsyncSenderMap::remove(AsyncSender* as, wsrep_seqno_t seqno)
{
    gu::Critical crit(monitor_);
    std::set<AsyncSender*>::iterator i(senders_.find(as));
    if (i == senders_.end())
    {
        throw gu::NotFound();
    }
    senders_.erase(i);
}


void galera::ist::AsyncSenderMap::cancel()
{
    gu::Critical crit(monitor_);
    while (senders_.empty() == false)
    {
        AsyncSender* as(*senders_.begin());
        senders_.erase(*senders_.begin());
        int err;
        as->cancel();
        monitor_.leave();
        if ((err = pthread_join(as->thread_, 0)) != 0)
        {
            log_warn << "pthread_join() failed: " << err;
        }
        monitor_.enter();
        delete as;
    }

}
