#pragma once

#include <device_handler.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/error.hpp>

#include <boost/date_time/posix_time/posix_time_config.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/system//error_code.hpp>
#include <boost/system/detail/error_code.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <iostream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

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
		request, std::function<void(boost::system::error_code,std::vector<uint8_t>)>callback);
    

private:
  struct ReadState {
        std::weak_ptr<DeviceHandler> weak_self;
        uint16_t request_count;
        std::shared_ptr<bool> is_completed;
        std::shared_ptr<std::vector<uint8_t>> responses;
        std::shared_ptr<std::vector<uint8_t>> header_buf;
        std::shared_ptr<std::vector<uint8_t>> current_response;
        std::function<void(boost::system::error_code ec, std::vector<uint8_t>)> callback;
        std::shared_ptr<uint16_t> iPtr;

        void read_next() {
            auto self = weak_self.lock();
            if (!self){
		    std::cerr << "retrun !self"; return;
	    }
            if (*is_completed) return;

            if (*iPtr >= request_count) {
                *is_completed = true;
                self->timer_timeout_.cancel();
                callback(boost::system::error_code(), *responses);
		self->finish_processing();
                return;
            }

            boost::asio::async_read(
                self->device_socket_,
                boost::asio::buffer(*header_buf),
                [self, this](boost::system::error_code ec, std::size_t) mutable {
                    if (!self) return;

                    if (*is_completed) return;
                    if (ec) {
                        *is_completed = true;
                        self->timer_timeout_.cancel();
                        callback(ec, {});
			self->finish_processing();
                        return;
                    }

		    std::cerr << std::endl << "header reaaded ";
                    size_t payload_len = ((*header_buf)[4] << 8) | (*header_buf)[5];
		    std::cerr << payload_len << std::endl;
                    current_response->resize(6 + payload_len);
                    std::copy_n(header_buf->begin(), 6, current_response->begin());

                    boost::asio::async_read(
                        self->device_socket_,
                        boost::asio::buffer(current_response->data() + 6, payload_len),
                        [self, this](boost::system::error_code ec, std::size_t bytes_read) mutable {
                            if (!self) return;

                            if (*is_completed) return;
                            if (ec) {
                                *is_completed = true;
                                self->timer_timeout_.cancel();
                                callback(ec, {});
				self->finish_processing();
                                return;
                            }

                            std::copy(current_response->begin(), 
                                    current_response->begin() + 6 + bytes_read, 
                                    std::back_inserter(*responses));
                            (*iPtr)++;

                            // Continue reading next response recursively
                            read_next();
                        });
                });
        }
    };
    DeviceHandler(boost::asio::io_context& ctx,
                 const boost::asio::ip::tcp::endpoint& device_endpoint);
    struct Request {
        std::vector<uint8_t> data;
        std::function<void(boost::system::error_code, std::vector<uint8_t>)> callback;
	uint16_t request_count;
    };
//    const size_t max_depth = 3;
    size_t depth = 0;
//    uint16_t i = 0;
    
     boost::posix_time::time_duration timeout_ = boost::posix_time::milliseconds(200);
    
    void async_write_read(uint16_t request_count, const std::vector<uint8_t>& data, std::function<void(boost::system::error_code,
                std::vector<uint8_t>)>callback);
    void async_read_n_responses(uint16_t request_count, std::function<void(boost::system::error_code,
                std::vector<uint8_t>)>callback);
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
    boost::asio::deadline_timer timer_timeout_;


    ReadState read_state;
    std::atomic<bool> is_processing_ = false;
    std::mutex queue_mutex_;
    std::queue<Request> request_queue_;
};
