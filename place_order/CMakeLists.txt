cmake_minimum_required(VERSION 3.16)
project(place_order CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(place_order
    src/main.cpp
    src/bybit_demo_client.cpp
)

# cppzmq header (zmq.hpp)
find_path(CPPZMQ_INCLUDE_DIR zmq.hpp)
if (CPPZMQ_INCLUDE_DIR)
    target_include_directories(place_order PRIVATE ${CPPZMQ_INCLUDE_DIR})
endif()

# libzmq
find_library(ZMQ_LIB zmq)
if (NOT ZMQ_LIB)
    message(FATAL_ERROR "libzmq not found. Install: sudo apt-get install libzmq3-dev cppzmq-dev")
endif()

target_include_directories(place_order PRIVATE include)

target_link_libraries(place_order PRIVATE
    ${ZMQ_LIB}
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)
