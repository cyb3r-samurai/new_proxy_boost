#include "client_session.h"
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <memory>
#include <numeric>
#include <server.h>

Server::Server(boost::asio::io_context& ctx, unsigned short port, 
        std::shared_ptr<DeviceHandler> device_handler) 
        : acceptor_(ctx, {boost::asio::ip::tcp::v4(), port})
        , device_handler_(device_handler) 
    {
        accept_connection();
    }

void Server::accept_connection() {
    acceptor_.async_accept([this] (boost::system::error_code ec,
            boost::asio::ip::tcp::socket clien_sock) {
                if (!ec) {
                    auto session = ClientSession::start(std::move(clien_sock), device_handler_);
                    sessions_.insert(session);

                    session->set_cleanup_callback([this, session] {
                                sessions_.erase(session);
                            });
                }
            accept_connection();
    });
}

