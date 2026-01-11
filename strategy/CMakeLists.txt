cmake_minimum_required(VERSION 3.16)
project(strategy_runner LANGUAGES CXX)

# -----------------------------
# C++ standard
# -----------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------
# Dependencies
# -----------------------------
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

# ZeroMQ (C library)
find_library(ZMQ_LIB
    NAMES zmq
    PATHS
        /usr/lib/x86_64-linux-gnu
        /lib/x86_64-linux-gnu
        /usr/local/lib
    NO_DEFAULT_PATH
)

if (NOT ZMQ_LIB)
    message(FATAL_ERROR "libzmq not found. Install with: sudo apt install libzmq3-dev")
endif()

message(STATUS "Using ZeroMQ lib: ${ZMQ_LIB}")

# -----------------------------
# Target
# -----------------------------
add_executable(strategy_runner
    src/main.cpp
    src/imbalance_taker.cpp
	src/zmq_market_subscriber.cpp
	src/virtual_wallet.cpp
	src/paper_execution_engine.cpp
)

# -----------------------------
# Includes
# -----------------------------
target_include_directories(strategy_runner
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# -----------------------------
# Link libraries
# -----------------------------
target_link_libraries(strategy_runner
    PRIVATE
        Threads::Threads
        nlohmann_json::nlohmann_json
        ${ZMQ_LIB}
)
