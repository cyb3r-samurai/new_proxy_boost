#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <device_handler.h>
#include <client_session.h>
#include <memory>
#include <server.h>
#include <thread>
#include <vector>

int  main () {
    boost::asio::io_context ctx; 
    
    boost::asio::ip::tcp::endpoint device_endpoint(
        boost::asio::ip::make_address("192.168.1.10"), 502
    );

    auto device_handler = DeviceHandler::create(ctx, device_endpoint);

    std::vector<std::unique_ptr<Server>> servers;

    for (auto port : {5020, 5021, 5022, 5023}) {
        servers.push_back(std::make_unique<Server>(ctx, port, device_handler));
    }

    const int thread_count = std::thread::hardware_concurrency();
    std::cout << "thread count is " << thread_count << std::endl;

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&ctx]  {ctx.run();});
    }

    for (auto& t: threads) {
        t.join();
    }

    return 0;
}
