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

void DeviceHandler::push_reqest(uint16_t request_count, std::vector<uint8_t>
		request, std::function<void(std::vector<uint8_t>)>callback) {
//	std::cout << "we in push request";

	boost::asio::post(strand_,[this, self = shared_from_this(),request_count, request = std::move(request), callback]{
			std::cerr << "post invoke" << std::endl;

			boost::asio::async_write(device_socket_, boost::asio::buffer(request),
			[&,this, self = shared_from_this(), request_count,  callback](boost::system::error_code ec, size_t){
				std::cerr<< std::endl <<request_count <<"device_handler after_write"<< std::endl;
	//			std::shared_ptr<std::vector<uint8_t>> all_responce_data = std::make_shared<std::vector<uint8_t>>();

			std::shared_ptr<std::vector<uint8_t>> all_responce_data = std::make_shared<std::vector<uint8_t>>();
			std::shared_ptr<uint16_t> i = std::make_shared<uint16_t>(0);

			std::cerr << "initialization of read next" << std::endl;	
			std::function<void()> read_next = ([&,self = shared_from_this(), this, all_responce_data, request_count,callback, read_next, i] () {
					std::cerr << std::endl << "i = " << *i << " n = " <<request_count<< std::endl;
					if (*i == request_count) {
						*i  = 0;

						std::cerr<< std::endl << "post execute callback"<< std::endl;
						callback(*all_responce_data);
					}
					else {
					std::shared_ptr<std::array<uint8_t,6>> recponce_buf = std::make_shared<std::array<uint8_t, 6>>();
					
					
					std::cerr <<"before post in read_next";

					std::cerr <<"in post in read_next";
					device_socket_.async_read_some(boost::asio::buffer(*recponce_buf),
						[&,self = shared_from_this(), i, this,recponce_buf, all_responce_data, request_count, callback, read_next](boost::system::error_code ec, size_t readed){
							if (ec || readed < 6) {
							      self->handle_request_error(ec);
							}
                    					const uint16_t pdu_len = ((*recponce_buf)[4] << 8 | (*recponce_buf)[5]);
							std::cerr <<std:: endl<< "we after read  header" << readed << "pdu len "  << pdu_len <<std::endl;
							std::shared_ptr<std::array<uint8_t, 320>> full_recponce = std::make_shared<std::array<uint8_t, 320>>();
							std::copy(recponce_buf->begin(), recponce_buf->end(), full_recponce->begin());


					std::cerr <<"before post 2  in read_next";
							device_socket_.async_read_some(boost::asio::buffer(full_recponce->data() + 6, pdu_len),
									[&,self = shared_from_this(), i, full_recponce, all_responce_data, request_count, callback, read_next](boost::system::error_code, size_t readed){										std::copy(full_recponce->begin(), full_recponce->begin() + readed + 6, std::back_inserter(*all_responce_data));
										std::cerr <<std::endl<< "full recponce size"<< readed<< " + 6 " 
										<< "we after copy " << all_responce_data->size() << " "<< *i << " " << request_count << std::endl ;
										(*i)++;
										std::cerr<< "we after increment";
										read_next();
									});

						});
					}
				});
			std::cerr << "initialization of read next end" << std::endl;	
				read_next();	
			});

		});

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
//                    handle_request_error(tid,  ec);
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
                            self->handle_request_error( ec ? ec : boost::asio::error::eof);
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
                self->handle_request_error( ec);
                return;
            }

            if (auto it = self->pending_responses_.find(tid); it != self->pending_responses_.end()) {
                it->second.callback(*response_buf);
                self->pending_responses_.erase(it);
            } 

            self->try_send_request();
        });

}

void DeviceHandler::handle_request_error( boost::system::error_code ec){

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


