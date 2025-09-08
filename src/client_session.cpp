#include <array>
#include <boost/asio/write.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/error_code.hpp>
#include <client_session.h>
#include <cstdint>
#include <iostream>
#include <memory>

ClientSession::ClientSession(boost::asio::ip::tcp::socket clien_sock,
            std::shared_ptr<DeviceHandler> device_handler ) 
            : client_sock_(std::move(clien_sock))
            ,  device_handler_(device_handler) {}


void ClientSession::read_header(std::shared_ptr<ClientSession> self) {
	std::cout << "read header called " << std::endl;
    auto header_ = std::make_shared<std::array<uint8_t, 6>>();
    client_sock_.async_read_some(boost::asio::buffer(*header_), 
            [this, self, header_](boost::system::error_code ec, size_t bytes_read) {
                if(ec || bytes_read < 6) {
                    handle_error(ec);
                    return;
                }

                tid = (header_->at(0) << 8) | header_->at(1);
                const uint16_t pdu_len = (header_->at(4) << 8) | header_->at(5);

                read_body(self, pdu_len, header_);
            });
}

void ClientSession::read_full_message(std::shared_ptr<ClientSession> self) {
    std::cout << "read_full_message_called";
    
    auto message_ = std::make_shared<std::vector<uint8_t>>();
    message_->resize(1100);
    client_sock_.async_read_some(boost::asio::buffer(*message_),
            [this, self, message_](boost::system::error_code ec, size_t bytes_read ){

                if(ec || bytes_read < 6) {
                    handle_error(ec);
                    return;
                }

                calculate_request_count(self, message_, bytes_read);
                });

}

void ClientSession::calculate_request_count(std::shared_ptr<ClientSession> self,
        std::shared_ptr<std::vector<uint8_t>> message, size_t bytes_readed) {
	std::cout << bytes_readed << std::endl;
        uint16_t bytes_reaminning = bytes_readed;
        uint16_t request_count = 0;
        auto lamda = [](std::array<uint8_t, 6> header)  -> uint16_t{
            return static_cast<uint16_t>(header[4] << 8 | header[5]);
        };  
        uint16_t current_index = 0;
        while (bytes_reaminning > 0) {
            std::array<uint8_t, 6> header;
            std::copy(message->begin() + current_index, message->begin() + current_index + 6,
                        header.begin());
            uint16_t pdu_len  = lamda(header);
//	    std::cout << pdu_len <<std::endl;
            request_count++;
            current_index = current_index + pdu_len + 6;
            bytes_reaminning = bytes_reaminning - pdu_len - 6;
	    std::cout << "one request readed";
        }
	message->resize(bytes_readed);
	std::cout << "we in ready to send push_request";
        std::cout << bytes_reaminning << " " << request_count<< std::endl;
	std::cout << "we in ready to send push_request";
	device_handler_->push_reqest(request_count, *message,
			[this,self](std::vector<uint8_t> recponse) {
				send_to_client(self, recponse);
			});
}

void ClientSession::read_body(std::shared_ptr<ClientSession>self, uint16_t pdu_len, std::shared_ptr<std::array<uint8_t, 6>>header_) {
    auto request_buf = std::make_shared<std::vector<uint8_t>>(pdu_len + 6);

    std::copy(header_->begin(), header_->end(), request_buf->begin());

    client_sock_.async_read_some(boost::asio::buffer(request_buf->data() + 6 , pdu_len),
            [this, self, request_buf](boost::system::error_code ec, size_t bytes_read) { 
                if (ec){
                    handle_error(ec);
                    return;
                }
		std::cout << "client message readed " << std::endl;
                device_handler_->enqueue_request(tid, *request_buf, 
                        [this, self](std::array<uint8_t, 256> response){
                            if(response.empty()) {
                                client_sock_.close();
                            } else {
                           //     send_to_client(self, response);
                            }
                        });

            });
}

void ClientSession::send_to_client(std::shared_ptr<ClientSession> self, std::vector<uint8_t>& response) {
	std::cerr << "we in send to client" << std::endl;
	std::cerr << response.size() << std::endl;
    //const uint16_t pdu_len = (response[4] << 8) | response[5];
    //const size_t total_size = 6 + pdu_len;
    //
	boost::asio::ip::tcp::endpoint remote_ep = client_sock_.remote_endpoint();
	std::cerr<< std::endl << "port " << (unsigned short)remote_ep.port() << std::endl;


    boost::asio::async_write(client_sock_, boost::asio::buffer(response),
        [this, self](boost::system::error_code ec, size_t){
            if (!ec) read_full_message(self);
        });
}

void ClientSession::handle_error(boost::system::error_code ec) {
    if (ec !=  boost::asio::error::eof) {
        std::cerr << "Client error: "<< ec.message() << "\n";
    }
    client_sock_.close();
}

ClientSession::~ClientSession() {
    if(cleanup_cb) cleanup_cb();
}
