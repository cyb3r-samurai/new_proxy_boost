#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/system//error_code.hpp>
#include <cstdint>
#include <unordered_map>
#include <queue>
#include <functional>
#include <memory>
#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

using TransactionId = uint16_t;

class DeviceHandler : public std::enable_shared_from_this<DeviceHandler> {
public:
    static std::shared_ptr<DeviceHandler>create(
            boost::asio::io_context& ctx,
            const boost::asio::ip::tcp::endpoint& device_endpoint) {
            std::shared_ptr<DeviceHandler> handler(new DeviceHandler(ctx, device_endpoint));

            boost::asio::post(handler->strand_, [handler] {handler->connect_to_device();});
            return handler;
    };
             
    void start();
    void enqueue_request(TransactionId tid,
                        std::vector<uint8_t> request,
                        std::function<void(std::array<uint8_t, 256>)>callback);

private:
    DeviceHandler(boost::asio::io_context& ctx,
                 const boost::asio::ip::tcp::endpoint& device_endpoint);
    struct Request {
        TransactionId tid;
        std::vector<uint8_t> data;
        std::function<void(std::array<uint8_t, 256>)> callback;
    };
    const size_t max_depth = 3;
    size_t depth = 0;
    
    
    void connect_to_device();
    void process_next_request();
    void start_request(const Request& request);
    void try_send_request();
    void read_device_response_header(TransactionId tid);
    void read_response_body(TransactionId tid, uint16_t pdu_len,
            std::shared_ptr<std::array<uint8_t, 6>>header_buf);
    void handle_device_error(boost::system::error_code ec);
    void retry_connection();
    void handle_request_error(TransactionId tid, boost::system::error_code ec);
    
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    boost::asio::io_context& ctx_;
    boost::asio::ip::tcp::endpoint device_endpoint_;
    boost::asio::ip::tcp::socket device_socket_;
    boost::asio::steady_timer timer_;
    bool is_connected_ = false;
    bool is_processing_ = false;


    std::queue<Request> request_queue_;
    std::array<uint8_t, 6> response_header_;
    std::unordered_map<TransactionId, Request> pending_responses_;
};
