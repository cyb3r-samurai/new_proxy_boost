#pragma once

#include "client_session.h"
#include <device_handler.h>
#include<boost/asio/io_context.hpp>
#include<boost/asio/ip/tcp.hpp>
#include <memory>
#include <unordered_set>




class Server {
public:
    Server(boost::asio::io_context& ctx, unsigned short port,
            std::shared_ptr<DeviceHandler> device_handler);

private:
    void accept_connection();

    std::unordered_set<std::shared_ptr<ClientSession>> sessions_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<DeviceHandler> device_handler_;
};
