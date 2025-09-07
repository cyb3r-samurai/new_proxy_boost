#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/is_buffer_sequence.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/connect.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <cstdint>
#include <device_handler.h>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>

DeviceHandler::DeviceHandler(boost::asio::io_context & ctx,
        const boost::asio::ip::tcp::endpoint& device_endpoint) 
    : ctx_(ctx)
      , strand_(boost::asio::make_strand(ctx.get_executor()))
      , device_endpoint_(device_endpoint)
      , device_socket_(strand_)
      , timer_ (strand_)
    { 
   //     connect_to_device();
    }

void DeviceHandler::start() {
    connect_to_device();
}

void DeviceHandler::enqueue_request(TransactionId tid, std::vector<uint8_t> request,
        std::function<void(std::array<uint8_t, 256>)> callback){
    auto self = shared_from_this();
    std::cout << "enqueue_request called" << std::endl;
    ctx_.post([this, self, tid, request = std::move(request), callback]{
                request_queue_.push({tid,request, callback});
                try_send_request();
            });
}

void DeviceHandler::try_send_request() {
    while (depth < max_depth && !request_queue_.empty()) {
	    std::cout << "depth is : " << depth  << std::endl;
        auto req = request_queue_.front();
        request_queue_.pop();

	start_request(req);
    }
}

void DeviceHandler::start_request(const Request& rec) {
    depth++;
    try_send_request();
	std::cout << " in start request depth is : " << depth  << std::endl;
    pending_responses_.emplace(rec.tid, rec);
    boost::asio::async_write(device_socket_, boost::asio::buffer(rec.data),
            [this, self = shared_from_this(), tid = rec.tid] (boost::system::error_code ec, size_t){
                if (ec) {
                    handle_request_error(tid,  ec);
                    return;
                }
                read_device_response_header(tid);
            });
}


void DeviceHandler::connect_to_device(){
    std::cerr << "we in connect_to_device";
    device_socket_.async_connect(device_endpoint_,
            [self = shared_from_this()](boost::system::error_code ec){
                if(!ec) {
                    self->is_connected_ = true;
                    self->try_send_request();
                } else {
                    self->retry_connection(); 
                }
            });
}

void DeviceHandler::read_device_response_header(TransactionId tid) {
        auto response_buf = std::make_shared<std::array<uint8_t, 6>>();

        device_socket_.async_read_some(boost::asio::buffer(*response_buf), 
                [self = shared_from_this(), tid, response_buf](boost::system::error_code ec, size_t bytes_read)
                    { 
                        if (ec || bytes_read < 6) {
                            self->handle_request_error(tid, ec ? ec : boost::asio::error::eof);
                            return;
                        }

                    const uint16_t pdu_len = (self->response_header_[4] << 8) | (*response_buf)[5];
                    self->read_response_body(tid, pdu_len, response_buf);
                    });
}

void DeviceHandler::read_response_body(TransactionId tid, uint16_t pdu_len, std::shared_ptr<std::array<uint8_t,6>> header_buf) {
    auto response_buf = std::make_shared<std::array<uint8_t, 256>>();

    std::copy(header_buf->begin(), header_buf->end(), response_buf->begin());
    
    device_socket_.async_read_some(boost::asio::buffer(response_buf->data() + 6, pdu_len),
        [self = shared_from_this(), tid, response_buf](boost::system::error_code ec, size_t bytes_read) {
            self->depth --;    

            if (ec) {
                self->handle_request_error(tid, ec);
                return;
            }

            if (auto it = self->pending_responses_.find(tid); it != self->pending_responses_.end()) {
                it->second.callback(*response_buf);
                self->pending_responses_.erase(it);
            } 

            self->try_send_request();
        });

}

void DeviceHandler::handle_request_error(TransactionId tid, boost::system::error_code ec){
    depth --;
    if(auto it = pending_responses_.find(tid); it != pending_responses_.end()) {
        it->second.callback({});
        pending_responses_.erase(it);
    }
    try_send_request();

    if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset) {
        retry_connection();
    };

}

void DeviceHandler::retry_connection() {
    timer_.expires_after(std::chrono::seconds(1));
    timer_.async_wait([self = shared_from_this()](boost::system::error_code) {
            self->connect_to_device();
            });
}


