#pragma once

#include "device_handler.h"
#include <array>
#include <algorithm>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <iostream>

#include <vector>
class ClientSession : std::enable_shared_from_this<ClientSession> {
public:
    static std::shared_ptr<ClientSession> start (boost::asio::ip::tcp::socket  client_sock,
            std::shared_ptr<DeviceHandler> device_handler) {

        auto session = std::shared_ptr<ClientSession>(
                new ClientSession(std::move(client_sock), device_handler));

        boost::asio::post(session->client_sock_.get_executor(),
                    [session] {session->read_full_message(session);});
        return session;
    }
    void set_cleanup_callback(std::function<void()> cb) {cleanup_cb = std::move(cb); }

    ~ClientSession();


private:
    ClientSession(boost::asio::ip::tcp::socket client_sock, 
            std::shared_ptr<DeviceHandler> device_handler);
    void send_to_client(std::shared_ptr<ClientSession> self,std::vector<uint8_t>& response);
    void handle_error(boost::system::error_code ec);
    void read_full_message(std::shared_ptr<ClientSession> self);
    void calculate_request_count(std::shared_ptr<ClientSession> self, std::shared_ptr<std::vector<uint8_t>>, size_t bytes_readed);
    
    std::function<void()> cleanup_cb;

    boost::asio::ip::tcp::socket client_sock_;
    std::shared_ptr<DeviceHandler> device_handler_;
    TransactionId tid;
};
