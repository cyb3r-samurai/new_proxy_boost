#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/detail/parsers.hpp>
#include <boost/program_options/option.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <device_handler.h>
#include <client_session.h>
#include <exception>
#include <memory>
#include <server.h>
#include <thread>
#include <vector>
namespace opt = boost::program_options;



int  main (int argc, char* argv[]) {

    opt::options_description desc("All options");

    desc.add_options()
        ("help", "Show help message")
        ("devices", opt::value<std::vector<std::string>>()->multitoken()->required(),"Devices IP (1 to 4))");

    opt::variables_map vm;

    try {
        opt::store(opt::parse_command_line(argc, argv , desc), vm);
        opt::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error: " <<e.what() << std::endl;
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

    if (vm.count("devices")) {
        const std::vector<std::string> & devices  = vm ["devices"].as<std::vector<std::string>>();
        size_t num_devices = devices.size();

        if (num_devices < 1 || num_devices  > 4) {
            std::cerr << "Error: Numver of servers must be between 1 and 4. Provided: " << num_devices  << std::endl;
            return 1;
        }

        boost::asio::io_context ctx; 

        std::vector<int> ports = {5020, 5021, 5022, 5023};
        std::vector<boost::asio::ip::tcp::endpoint> devices_endpoints;
        std::vector<std::unique_ptr<Server>> servers;
        std::vector<std::shared_ptr<DeviceHandler>> handlers;

        for (size_t i = 0; i < num_devices; ++i){

            boost::asio::ip::tcp::endpoint device_endpoint(
                boost::asio::ip::make_address(devices[i]), 502
            );

            auto device_handler = DeviceHandler::create(ctx, device_endpoint);
            handlers.push_back(device_handler);
            servers.push_back(std::make_unique<Server>(ctx, ports[i], device_handler));
        }
        const int thread_count = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ctx]  {ctx.run();});
        }

        for (auto& t: threads) {
            t.join();
        }

        return 0;
    }
    return 0;
}
