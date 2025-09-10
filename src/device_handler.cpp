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
#include <algorithm>
#include <iterator>

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


void DeviceHandler::async_write_read(uint16_t request_count, const std::vector<uint8_t>& data,
		std::function<void(std::vector<uint8_t>)> callback) {

	boost::asio::async_write (
			device_socket_,
			boost::asio::buffer(data),

			[self = shared_from_this(), callback, request_count] (boost::system::error_code ec, std::size_t) {

				self->async_read_n_responses(request_count, callback);
			}
			);



}
void DeviceHandler::async_read_n_responses(uint16_t request_count, std::function<void(std::vector<uint8_t>)> callback) {

	auto responses = std::make_shared<std::vector<uint8_t>>();
	auto header_buf = std::make_shared<std::vector<uint8_t>>(6);
	auto current_response = std::make_shared<std::vector<uint8_t>>();
	auto iPtr =std::make_shared<uint16_t>(0);

	 auto read_next_ptr = std::make_shared<std::function<void()>>();
	*read_next_ptr = [this, request_count, responses, header_buf, current_response, callback, iPtr, read_next_ptr]() mutable {

		std::cerr << std::endl << "i = " << *iPtr << "request_count = " << request_count;
		if (*iPtr == request_count) {
			callback(*responses);
			finish_processing();
			return;
		}


		boost::asio::async_read (
				device_socket_,
				boost::asio::buffer(*header_buf),
				[this, request_count, responses, header_buf, current_response, callback, read_next_ptr, iPtr] (
						boost::system::error_code ec, std::size_t)mutable {
					size_t payload_len = ((*header_buf)[4] << 8) | (*header_buf)[5];
					current_response->resize(6+payload_len);
					std::copy_n(header_buf->begin(), 6, current_response->begin());

					boost::asio::async_read(
							device_socket_,
							boost::asio::buffer(current_response->data()+6, payload_len),
							[this, request_count, responses, header_buf, current_response, callback, read_next_ptr, iPtr](
								boost::system::error_code ec, std::size_t bytes_readed)mutable{
						
							std::copy(current_response->begin(), current_response->begin()+6+bytes_readed, std::back_inserter(*responses));

							(*iPtr) ++;
							(*read_next_ptr)();

							});

				}

				);
	};

	(*read_next_ptr)();

}

void DeviceHandler::push_reqest(uint16_t request_count, std::vector<uint8_t>
		data, std::function<void(std::vector<uint8_t>)>callback) {
//	std::cout << "we in push request";
//

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
    std::cerr << "we in connect_to_device";
    device_socket_.async_connect(device_endpoint_,
            [self = shared_from_this()](boost::system::error_code ec){
                if(!ec) {
                    self->is_connected_ = true;
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
	if (is_processing_) {
		return;
	}

	std::function<void(std::vector<uint8_t>)> callback;
	std::vector<uint8_t> data;
	uint16_t request_count;
	{

	std::lock_guard<std::mutex> lock(queue_mutex_);
	{
		if(request_queue_.empty()){
			return;
		}
	}

	auto request = request_queue_.front();
	request_queue_.pop();
	data = request.data;
	callback = request.callback;
	request_count = request.request_count;
	is_processing_ = true;

	}
	async_write_read(request_count, data, callback);

}

void DeviceHandler::retry_connection() {
    timer_.expires_after(std::chrono::seconds(1));
    timer_.async_wait([self = shared_from_this()](boost::system::error_code) {
            self->connect_to_device();
            });
}


