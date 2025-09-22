#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/detail/is_buffer_sequence.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/date_time/posix_time/posix_time_config.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <device_handler.h>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>

DeviceHandler::DeviceHandler(
    boost::asio::io_context &ctx,
    const boost::asio::ip::tcp::endpoint &device_endpoint)
    : ctx_(ctx), strand_(boost::asio::make_strand(ctx.get_executor())),
      device_endpoint_(device_endpoint), device_socket_(strand_),
      timer_(strand_), timer_timeout_(strand_) {
  //     connect_to_device();
}

void DeviceHandler::start() { connect_to_device(); }

void DeviceHandler::async_write_read(
    uint16_t request_count, const std::vector<uint8_t> &data,
    std::function<void(boost::system::error_code ec, std::vector<uint8_t>)>
        callback) {

  boost::asio::async_write(device_socket_, boost::asio::buffer(data),

                           [self = shared_from_this(), callback, request_count](
                               boost::system::error_code ec, std::size_t) {
                             if (ec) {
                               callback(ec, {});
                               self->finish_processing();
                               return;
                             }

                             self->async_read_n_responses(request_count,
                                                          callback);
                           });
}

void DeviceHandler::async_read_n_responses(
    uint16_t request_count,
    std::function<void(boost::system::error_code ec, std::vector<uint8_t>)> callback) {

    // общие буферы/счётчики, живут пока живут колбэки
    auto responses = std::make_shared<std::vector<uint8_t>>();
    auto header_buf = std::make_shared<std::array<uint8_t,6>>();
    auto current_response = std::make_shared<std::vector<uint8_t>>();
    auto read_count = std::make_shared<uint16_t>(0);
    auto is_completed = std::make_shared<bool>(false);

    std::weak_ptr<DeviceHandler> weak_self = shared_from_this();

    // Армируем таймаут для всей операции (можно менять логику — таймаут на каждое чтение)
        timer_timeout_.expires_from_now(timeout_);
        timer_timeout_.async_wait([weak_self, is_completed, responses, callback](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) return; // таймер отменён нормальным завершением
        if (*is_completed) return;
        *is_completed = true;
        if (auto self = weak_self.lock()) {
            // отменяем все операции на сокете
            self->device_socket_.cancel();
            // возвращаем таймаут как ошибку
            callback(boost::system::error_code(boost::asio::error::timed_out), *responses);
            self->finish_processing();
        }
    });

    // рекурсивный вызов через shared_ptr/weak_ptr (чтобы не было самоссылок)
    auto read_next_ptr = std::make_shared<std::function<void(const boost::system::error_code&)>>();
    std::weak_ptr<std::function<void(const boost::system::error_code&)>> weak_read_next = read_next_ptr;

    *read_next_ptr = [weak_self, header_buf, current_response, responses, read_count, is_completed, request_count, callback, weak_read_next, this](const boost::system::error_code& ec) mutable {
        if (*is_completed) return;

        if (ec) {
            // ошибка от предыдущей операции
            if (auto self = weak_self.lock()) {
                *is_completed = true;
                self->timer_timeout_.cancel();
                callback(ec, {});
                self->finish_processing();
            }
            return;
        }

        // если уже прочли все ответы
        if (*read_count >= request_count) {
            if (auto self = weak_self.lock()) {
                *is_completed = true;
                self->timer_timeout_.cancel();
                callback(boost::system::error_code(), *responses);
                // опционально освободить память
                responses->clear();
                responses->shrink_to_fit();
                self->finish_processing();
            }
            return;
        }

        // читаем заголовок ровно 6 байт
        if (auto self = weak_self.lock()) {
            boost::asio::async_read(self->device_socket_, boost::asio::buffer(header_buf->data(), 6),
                [weak_self, header_buf, current_response, responses, read_count, is_completed, request_count, callback, weak_read_next](const boost::system::error_code& ec, std::size_t bytes_transferred) mutable {
                    if (*is_completed) return;
                    if (ec) {
                        if (auto self = weak_self.lock()) {
                            *is_completed = true;
                            self->timer_timeout_.cancel();
                            callback(ec, {});
                            self->finish_processing();
                        }
                        return;
                    }
                    if (bytes_transferred != 6) {
                        if (auto self = weak_self.lock()) {
                            *is_completed = true;
                            self->timer_timeout_.cancel();
                            callback(boost::system::error_code(boost::asio::error::fault), {});
                            self->finish_processing();
                        }
                        return;
                    }

                    // наглядный лог (вставь по необходимости)
                     std::cerr << "Header read ok\n";

                    uint16_t payload_len = static_cast<uint16_t>(((uint16_t)((*header_buf)[4]) << 8) | (*header_buf)[5]);

                    // подготовим буфер пакета: заголовок + payload_len
                    current_response->resize(6 + payload_len);
                    std::copy_n(header_buf->begin(), 6, current_response->begin());

                    if (auto self2 = weak_self.lock()) {
                        // читаем ровно payload_len байт
                        boost::asio::async_read(self2->device_socket_, boost::asio::buffer(current_response->data() + 6, payload_len),
                            [weak_self, header_buf, current_response, responses, read_count, is_completed, request_count, callback, weak_read_next](const boost::system::error_code& ec, std::size_t payload_read) mutable {
                                if (*is_completed) return;
                                if (ec) {
                                    if (auto self = weak_self.lock()) {
                                        *is_completed = true;
                                        self->timer_timeout_.cancel();
                                        callback(ec, {});
                                        self->finish_processing();
                                    }
                                    return;
                                }

                                // payload_read должен равняться payload_len
                                if (payload_read != (current_response->size() - 6)) {
                                    if (auto self = weak_self.lock()) {
                                        *is_completed = true;
                                        self->timer_timeout_.cancel();
                                        callback(boost::system::error_code(boost::asio::error::fault), {});
                                        self->finish_processing();
                                    }
                                    return;
                                }

                                // добавляем весь пакет (6 + payload_len) в результирующий буфер
                                responses->insert(responses->end(), current_response->begin(), current_response->end());

                                ++(*read_count);

                                // отладочный вывод (по желанию)
                                 std::cerr << "Read packet " << *read_count << "/" << request_count
                                           << " payload_len=" << (current_response->size() - 6)
                                           << " total_bytes=" << responses->size() << "\n";

                                // продолжаем цикл — безопасный вызов через weak_ptr
                                if (auto read_next_locked = weak_read_next.lock()) {
                                    (*read_next_locked)(boost::system::error_code());
                                }
                            });
                    }
                });
        }
    };

    // стартуем
    (*read_next_ptr)(boost::system::error_code());
}



void DeviceHandler::push_reqest(
    uint16_t request_count, std::vector<uint8_t> data,
    std::function<void(boost::system::error_code, std::vector<uint8_t>)>
        callback) {

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    Request r;
    r.data = data;
    r.callback = callback;
    r.request_count = request_count;
    request_queue_.push(r);
  }

  boost::asio::post(
      strand_, [self = shared_from_this()]() { self->process_next_request(); });
}

void DeviceHandler::connect_to_device() {
  device_socket_.async_connect(
      device_endpoint_,
      [self = shared_from_this()](boost::system::error_code ec) {
        if (!ec) {
          self->is_connected_ = true;
          std::cerr << "\n"
                    << "Connected to " << self->device_endpoint_.address()
                    << ".\n";
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

void DeviceHandler::handle_request_error(boost::system::error_code ec) {

  if (ec == boost::asio::error::eof ||
      ec == boost::asio::error::connection_reset) {
    retry_connection();
  };
}

void DeviceHandler::process_next_request() {
  if (is_processing_) {
    return;
  }

  std::function<void(boost::system::error_code ec, std::vector<uint8_t>)>
      callback;
  std::vector<uint8_t> data;
  uint16_t request_count;
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
    is_processing_ = true;
  }

  if (!device_socket_.is_open()) {
    boost::system::error_code not_connected(boost::asio::error::not_connected);
    if (callback)
      callback(not_connected, {});
    retry_connection();
    return;
  }
  async_write_read(request_count, data, callback);
}

void DeviceHandler::retry_connection() {
  std::cerr << "\n"
            << "error recconnect to " << this->device_endpoint_.address()
            << ".\n";
  timer_.expires_after(std::chrono::seconds(1));
  timer_.async_wait([self = shared_from_this()](boost::system::error_code) {
    self->connect_to_device();
  });
}
