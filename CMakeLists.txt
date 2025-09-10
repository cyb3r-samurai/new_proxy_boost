cmake_minimum_required(VERSION 3.12)
project(modbus-proxy-boost LANGUAGES CXX)

find_package(Boost 1.70 REQUIRED COMPONENTS system thread program_options)

include_directories (
    ${PROJECT_SOURCE_DIR}/include
)
set (SOURCES src/main.cpp src/server.cpp src/device_handler.cpp src/client_session.cpp)
add_executable(proxy-boost ${SOURCES})

target_link_libraries(proxy-boost PRIVATE
    Boost::boost #header only asio 
    Boost::system # required for error handling
    Boost::program_options
)

target_compile_features(proxy-boost PRIVATE cxx_std_17)
