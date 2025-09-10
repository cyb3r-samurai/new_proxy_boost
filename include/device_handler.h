#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/system//error_code.hpp>
#include <cstdint>
#include <unordered_map>
#include <queue>
#include <mutex>
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
void push_reqest(uint16_t request_count, std::vector<uint8_t>
		request, std::function<void(std::vector<uint8_t>)>callback);
    

private:
    DeviceHandler(boost::asio::io_context& ctx,
                 const boost::asio::ip::tcp::endpoint& device_endpoint);
    struct Request {
        std::vector<uint8_t> data;
        std::function<void(std::vector<uint8_t>)> callback;
	uint16_t request_count;
    };
    const size_t max_depth = 3;
    size_t depth = 0;
//    uint16_t i = 0;
    
    
    void async_write_read(uint16_t request_count, const std::vector<uint8_t>& data, std::function<void(std::vector<uint8_t>)>callback);
    void async_read_n_responses(uint16_t request_count, std::function<void(std::vector<uint8_t>)>callback);
    void connect_to_device();
    void process_next_request();
    void try_send_request();
    void handle_device_error(boost::system::error_code ec);
    void retry_connection();
    void handle_request_error( boost::system::error_code ec);
    void finish_processing();
    
//    std::function<void()>read_next;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    boost::asio::io_context& ctx_;
    boost::asio::ip::tcp::endpoint device_endpoint_;
    boost::asio::ip::tcp::socket device_socket_;
    boost::asio::steady_timer timer_;
    bool is_connected_ = false;


    bool is_processing_ = false;
    std::mutex queue_mutex_;
    std::queue<Request> request_queue_;
};
