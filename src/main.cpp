#include <device_handler.h>
#include <client_session.h>
#include <server.h>

#include <boost/filesystem/path.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include <exception>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <iostream>


namespace opt = boost::program_options;
namespace fs  = boost::filesystem;



std::string getDefaultConfigPath() {
    const char* homeDir = getenv("HOME");
    if (!homeDir) { 
        throw std::runtime_error("Не удалось определить домашнюю директорию");
    }
    fs::path  configDir  = fs::path(homeDir)/".config"/ "modbus-proxy";
    fs::create_directory(configDir);
    return (configDir / "proxy.conf").string();
}

void createDefaultConfig(const std::string& configPath) {
    std::ofstream configFile(configPath);
    std::vector<int> ports = {5020, 5021, 5022, 5023};
    std::vector<std::string> ip = {"192.168.1.10", "192.168.1.11", "192.168.1.12", "192.168.1.13"};
    if (configFile) {
        configFile << " # Конфигуарационный файл приложения \n\n";
        for (size_t i = 0; i <ip.size(); ++i) {
            configFile << "deviceIP = ";
            configFile << ip[i];
            configFile << "\n";
        }
        for (size_t i = 0; i <ports.size(); ++i) {
            configFile << "port = ";
            configFile << ports[i];
            configFile << "\n";
        }
        configFile << "\n";
        std::cout << "Создан кофигурационный файл по умолчанию: "  << configPath << std::endl;
    } else {
        std::cerr << "Не удалось создать конфигурационный файл: "  << configPath << std::endl;
    }

}

int  main (int argc, char* argv[]) {

    opt::options_description cmdlineOptions("Комндная строка");

    cmdlineOptions.add_options()
        ("help", "Show help message")
    ;

    opt::options_description configOptions("Конфигурационные опции");
    configOptions.add_options()
        ("deviceIP", opt::value<std::vector<std::string>>()->multitoken()->required(),"IP адреса устройств")
        ("port", opt::value<std::vector<int>>()->multitoken()->required(),"Порты на localhost для подключения к каждому устройству соответственно")
    ;


    opt::variables_map vm;

    std::string configPath = getDefaultConfigPath();
    if (!fs::exists(configPath)) {
        createDefaultConfig(configPath);
    }

    std::ifstream configFile (configPath.c_str());
    if (configFile) {
        opt::store(opt::parse_config_file(configFile, configOptions), vm);
    }

    opt::store(opt::command_line_parser(argc, argv).options(cmdlineOptions).run(),vm); 
    opt::notify(vm);

    if (vm.count("help")) {
        std::cout << cmdlineOptions<< "\n" << configOptions << std::endl;
        return 0;
    }

    if (vm.count("deviceIP")) {
        const std::vector<std::string> & devices  = vm ["deviceIP"].as<std::vector<std::string>>();
        size_t num_devices = devices.size();

        if (num_devices < 1 || num_devices  > 4) {
            std::cerr << "Ошибка количество устройств должно быть от 1 до 4 получено " << num_devices  << std::endl;
            return 1;
        }

        const std::vector<int> & ports = vm["port"].as<std::vector<int>>();
        size_t num_ports = ports.size();
        if (num_devices != num_ports) {
            std::cerr << "Ошибка количетсво портов не совподает с количеством  устройств. Получено устройств " << num_devices <<" портов " << num_ports << std::endl;
            return 1;

        }

        boost::asio::io_context ctx; 

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
            std::cerr << "\n"<< "Connections to " <<devices[i] << " accepting in 127.0.0.1:" << ports[i] << ".\n";
        }
        const int thread_count = 16;
        std::vector<std::thread> threads;
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ctx]  {ctx.run();});
        }

        for (auto& t: threads) {
            t.join();
        }

        return 0;
    }
    else {

        std::cerr << "Ошибка не получены адреса устройств. " << std::endl;
        return 1;
    }

    return 0;
}
