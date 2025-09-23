#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/is_buffer_sequence.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/connect.hpp>
#include <boost/date_time/posix_time/posix_time_config.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <cstdint>
#include <device_handler.h>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <algorithm>
#include <iterator>

DeviceHandler::DeviceHandler(boost::asio::io_context & ctx,
        const boost::asio::ip::tcp::endpoint& device_endpoint) 
    : ctx_(ctx)
      , strand_(boost::asio::make_strand(ctx.get_executor()))
      , device_endpoint_(device_endpoint)
      , device_socket_(strand_)
      , timer_ (strand_)
      ,timer_timeout_(strand_)
    { 
   //     connect_to_device();
    }

void DeviceHandler::start() {
    connect_to_device();
}


void DeviceHandler::async_write_read(uint16_t request_count, const std::vector<uint8_t>& data,
		std::function<void(boost::system::error_code ec, std::vector<uint8_t>)> callback) {

    std::weak_ptr<DeviceHandler> weak_self = shared_from_this();

	boost::asio::async_write(
		device_socket_,
		boost::asio::buffer(data),
		[weak_self, callback, request_count](boost::system::error_code ec, std::size_t) {
            auto self = weak_self.lock();
            if (!self) return;

            if (ec) {
                callback(ec, {});
                self->finish_processing();
                return;
            }

			self->async_read_n_responses(request_count, callback);
		}
	);
}
void DeviceHandler::async_read_n_responses(uint16_t request_count, std::function<void(boost::system::error_code ec,
            std::vector<uint8_t>)> callback) {

    auto responses = std::make_shared<std::vector<uint8_t>>();
    auto header_buf = std::make_shared<std::vector<uint8_t>>(6);
    auto current_response = std::make_shared<std::vector<uint8_t>>();
    auto iPtr = std::make_shared<uint16_t>(0);
    auto is_completed = std::make_shared<bool>(false);

    // Use weak_ptr to avoid circular reference
    std::weak_ptr<DeviceHandler> weak_self = shared_from_this();

    timer_timeout_.expires_from_now(timeout_);
    timer_timeout_.async_wait([weak_self, callback, is_completed, responses](const boost::system::error_code& ec) {
        auto self = weak_self.lock();
        if (!self) return;

        if (ec == boost::asio::error::operation_aborted) return;
        if (*is_completed) return;
        *is_completed = true;
        self->device_socket_.cancel();
        callback(boost::system::error_code(), *responses);
        self->finish_processing();
    });

    // Create a recursive lambda using a shared state object
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
            if (!self) return;

            if (*is_completed) return;

            if (*iPtr >= request_count) {
                *is_completed = true;
                self->timer_timeout_.cancel();
                callback(boost::system::error_code(), *responses);
                self->finish_processing();
                return;
            }

            auto state = std::make_shared<ReadState>(*this);
            boost::asio::async_read(
                self->device_socket_,
                boost::asio::buffer(*header_buf),
                [state](boost::system::error_code ec, std::size_t) mutable {
                    auto self = state->weak_self.lock();
                    if (!self) return;

                    if (*state->is_completed) return;
                    if (ec) {
                        *state->is_completed = true;
                        self->timer_timeout_.cancel();
                        state->callback(ec, {});
                        self->finish_processing();
                        return;
                    }

                    size_t payload_len = ((*state->header_buf)[4] << 8) | (*state->header_buf)[5];
                    state->current_response->resize(6 + payload_len);
                    std::copy_n(state->header_buf->begin(), 6, state->current_response->begin());

                    boost::asio::async_read(
                        self->device_socket_,
                        boost::asio::buffer(state->current_response->data() + 6, payload_len),
                        [state](boost::system::error_code ec, std::size_t bytes_read) mutable {
                            auto self = state->weak_self.lock();
                            if (!self) return;

                            if (*state->is_completed) return;
                            if (ec) {
                                *state->is_completed = true;
                                self->timer_timeout_.cancel();
                                state->callback(ec, {});
                                self->finish_processing();
                                return;
                            }

                            std::copy(state->current_response->begin(), 
                                    state->current_response->begin() + 6 + bytes_read, 
                                    std::back_inserter(*state->responses));
                            (*state->iPtr)++;

                            // Continue reading next response recursively
                            state->read_next();
                        });
                });
        }
    };

    auto state = std::make_shared<ReadState>();
    state->weak_self = weak_self;
    state->request_count = request_count;
    state->is_completed = is_completed;
    state->responses = responses;
    state->header_buf = header_buf;
    state->current_response = current_response;
    state->callback = callback;
    state->iPtr = iPtr;

    // Start reading responses
    state->read_next();
}

void DeviceHandler::push_reqest(uint16_t request_count, std::vector<uint8_t>
		data, std::function<void(boost::system::error_code,std::vector<uint8_t>)>callback) {

	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		Request r;
		r.data = data;
		r.callback = callback;
		r.request_count = request_count;
		request_queue_.push(r);
	}

	boost::asio::post(strand_, 
			[self = shared_from_this()]() {
				self->process_next_request();
			});

}

void DeviceHandler::connect_to_device(){
    device_socket_.async_connect(device_endpoint_,
            [self = shared_from_this()](boost::system::error_code ec){
                if(!ec) {
                    self->is_connected_ = true;
                    std::cerr<< "\n" << "Connected to " << self->device_endpoint_.address() << ".\n";
		    self->process_next_request();
                   // self->try_send_request();
                } else {
                    self->retry_connection(); 
                }
            });
}


void DeviceHandler::finish_processing() {
	is_processing_ = false;
	process_next_request();
}

void DeviceHandler::handle_request_error( boost::system::error_code ec){

    if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset) {
        retry_connection();
    };

}

void DeviceHandler::process_next_request() {
	// Ensure this runs on the strand to prevent concurrent access
	if (!strand_.running_in_this_thread()) {
		boost::asio::post(strand_, [self = shared_from_this()]() {
			self->process_next_request();
		});
		return;
	}

	if (is_processing_) {
		return;
	}

	std::function<void(boost::system::error_code ec, std::vector<uint8_t>)> callback;
	std::vector<uint8_t> data;
	uint16_t request_count;
	{
	    std::lock_guard<std::mutex> lock(queue_mutex_);

        if(request_queue_.empty()){
			return;
		}

	    auto request = request_queue_.front();
	    request_queue_.pop();
	    data = std::move(request.data);
	    callback = std::move(request.callback);
	    request_count = request.request_count;
	    is_processing_ = true;
	}

    if(!device_socket_.is_open()) {
        boost::system::error_code not_connected(boost::asio::error::not_connected);
        if (callback) callback(not_connected, {});
        finish_processing();
        return;
    }

	async_write_read(request_count, data, callback);
}

void DeviceHandler::retry_connection() {
    std::cerr << "\n" << "error recconnect to "<< this->device_endpoint_.address() << ".\n";
    timer_.expires_after(std::chrono::seconds(1));
    timer_.async_wait([self = shared_from_this()](boost::system::error_code) {
            self->connect_to_device();
            });
}

