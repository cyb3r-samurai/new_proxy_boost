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
#include <boost/bind.hpp>
#include <cstddef>
#include <cstdint>
#include <device_handler.h>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <algorithm>
#include <iterator>

DeviceHandler::DeviceHandler(boost::asio::io_context& ctx,
                             const boost::asio::ip::tcp::endpoint& device_endpoint)
    : ctx_(ctx),
      strand_(boost::asio::make_strand(ctx.get_executor())),
      device_endpoint_(device_endpoint),
      device_socket_(strand_),
      timer_(strand_),
      timer_timeout_(strand_),
      connect_timeout_timer_(strand_) {
    //     connect_to_device();
}
void DeviceHandler::cheack_deadline() {
    if (stopped_)
        return;
    if (connect_timeout_timer_.expires_at() <= boost::asio::deadline_timer::traits_type::now()) {
        std::cerr<< "cheack_deadline expiress \n";
        device_socket_.close();
        connect_timeout_timer_.expires_at(boost::posix_time::pos_infin);
    }
    connect_timeout_timer_.async_wait(boost::bind(&DeviceHandler::cheack_deadline, this));
}

void DeviceHandler::start() {
    connect_timeout_timer_.async_wait(boost::bind(&DeviceHandler::cheack_deadline,this));
    connect_to_device();
}

void DeviceHandler::async_write_read(
    uint16_t request_count, std::vector<uint16_t> tids, const std::vector<uint8_t>& data,
    std::function<void(boost::system::error_code ec, std::vector<uint8_t>)> callback) {

    boost::asio::async_write(device_socket_, boost::asio::buffer(data),

                             [self = shared_from_this(), callback, request_count, data,tids](
                                 boost::system::error_code ec, std::size_t) {
                                 if (ec) {
                                     std::cerr << "error message in write: " << ec.message() << '\n';
                                     self->device_socket_.close();
                                     std::cerr << "close socket_ \n";
                                     self->finish_processing();
                                //     Request r;
                               //      r.callback  = callback;
                               //r.data = data;
                               //      r.request_count = request_count;
                               //      r.tids = tids;
                               //      self->request_queue_.push(r);
                               //      callback(ec, {});
                                     return;
                                 } else {
                                    self->async_read_n_responses(request_count, tids, callback);
                                 }
                             });
}
void DeviceHandler::async_read_n_responses(
    uint16_t request_count, std::vector<uint16_t> tids,
    std::function<void(boost::system::error_code ec, std::vector<uint8_t>)> callback) {
//    std::cerr << std::endl << "new async read started" << std::endl;

    auto responses = std::make_shared<std::vector<uint8_t>>();
    auto header_buf = std::make_shared<std::vector<uint8_t>>(6);
    auto current_response = std::make_shared<std::vector<uint8_t>>();
    auto iPtr = std::make_shared<uint16_t>(0);
    auto is_completed = std::make_shared<bool>(false);

    // Use weak_ptr to avoid circular reference
    std::weak_ptr<DeviceHandler> weak_self = shared_from_this();

    timer_timeout_.expires_from_now(timeout_);
    timer_timeout_.async_wait(
        [weak_self, callback, is_completed, responses](const boost::system::error_code& ec) {
            auto self = weak_self.lock();
            if (!self)
                return;


            if (ec == boost::asio::error::operation_aborted)
                return;
            if (*is_completed)
                return;
            *is_completed = true;
            self->device_socket_.cancel();
            std::cerr << "timer_timeout_ callback called" << " responses size:" << responses->size()
                      << std::endl;
            if (responses->size() > 0) {
                callback(boost::system::error_code(), *responses);
            }
            self->finish_processing();
        });

    // Create a recursive lambda using a shared state object

    read_state.weak_self = weak_self;
    read_state.request_count = request_count;
    read_state.is_completed = is_completed;
    read_state.responses = responses;
    read_state.header_buf = header_buf;
    read_state.current_response = current_response;
    read_state.callback = callback;
    read_state.iPtr = iPtr;
    read_state.tids = tids;

    // Start reading responses
    read_state.read_next();
}

void DeviceHandler::push_reqest(
    uint16_t request_count, std::vector<uint16_t> tids, std::vector<uint8_t> data,
    std::function<void(boost::system::error_code, std::vector<uint8_t>)> callback) {

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        Request r;
        r.tids = tids;
        r.data = data;
        r.callback = callback;
        r.request_count = request_count;
        request_queue_.push(r);
    }

    boost::asio::post(strand_, [self = shared_from_this()]() { self->process_next_request(); });
}


void DeviceHandler::connect_to_device() {
    std::cerr << "try to connnect to " << device_endpoint_.address().to_string() << std::endl;
    connect_timeout_timer_.expires_from_now(boost::posix_time::seconds(10));
    device_socket_.async_connect(
        device_endpoint_, [self = shared_from_this()](boost::system::error_code ec) {
            if (self->stopped_)
                return;
            if (!self->device_socket_.is_open()) {
                std::cerr << "Connect timedout\n";
                self->retry_connection();
            } else if (ec) {
                std::cerr << "Connection error: " << ec.message() << '\n';
                self->device_socket_.close();
                self->retry_connection();
            } else {
                self->is_connected_ = true;
                std::cerr << "\n" << "Connected to " << self->device_endpoint_.address() << ".\n";
                self->process_next_request();
                self->connect_timeout_timer_.expires_at(boost::posix_time::pos_infin);
            }
        });
}

void DeviceHandler::finish_processing() {
    is_processing_ = false;
    process_next_request();
}

void DeviceHandler::handle_request_error(boost::system::error_code ec) {

    if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset) {
        retry_connection();
    };
}

void DeviceHandler::process_next_request() {
    if (!strand_.running_in_this_thread()) {
        boost::asio::post(strand_, [self = shared_from_this()]() { self->process_next_request(); });
        return;
    }
    if (is_processing_) {
        return;
    }
    if (!device_socket_.is_open()) {
        connect_to_device();
        return;
    }

    std::function<void(boost::system::error_code ec, std::vector<uint8_t>)> callback;
    std::vector<uint8_t> data;
    uint16_t request_count;
    std::vector<uint16_t> tids;
    {

        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (request_queue_.empty()) {
            return;
        }

        auto request = request_queue_.front();
        request_queue_.pop();
        data = request.data;
        callback = request.callback;
        request_count = request.request_count;
        tids = request.tids;
        is_processing_ = true;
    }
    async_write_read(request_count, tids, data, callback);
}

void DeviceHandler::retry_connection() {
    std::cerr << "\n" << "error recconnect to " << this->device_endpoint_.address() << ".\n";
    timer_.expires_after(std::chrono::seconds(5));
    timer_.async_wait(
        [self = shared_from_this()](boost::system::error_code) { self->connect_to_device(); });
}
