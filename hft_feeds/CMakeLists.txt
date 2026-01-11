cmake_minimum_required(VERSION 3.16)
project(hft_feeds LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------
# Dependencies (safe ones)
# -----------------------------
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# -----------------------------
# Boost: MANUAL, BULLETPROOF
# -----------------------------
find_path(BOOST_INCLUDE_DIR
    NAMES boost/thread.hpp
    PATHS /usr/include /lib/x86_64-linux-gnu
    NO_DEFAULT_PATH
)

find_library(BOOST_THREAD_LIB
    NAMES boost_thread
    PATHS /lib/x86_64-linux-gnu
    NO_DEFAULT_PATH
)

find_library(BOOST_SYSTEM_LIB
    NAMES boost_system
    PATHS /lib/x86_64-linux-gnu
    NO_DEFAULT_PATH
)

if(NOT BOOST_INCLUDE_DIR OR NOT BOOST_THREAD_LIB OR NOT BOOST_SYSTEM_LIB)
    message(FATAL_ERROR "Boost system/thread libraries not found")
endif()

# -----------------------------
# Target
# -----------------------------
add_executable(hft_feeds
    src/main.cpp
    src/BinanceL2Feed.cpp
    src/BybitL2Feed.cpp
    src/MarketDataManager.cpp
    src/storage/StateDB.cpp
)

# -----------------------------
# Includes
# -----------------------------
target_include_directories(hft_feeds
    PRIVATE
        ${BOOST_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
)

# -----------------------------
# Link libraries
# -----------------------------
target_link_libraries(hft_feeds
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite::SQLite3
        ${BOOST_SYSTEM_LIB}
        ${BOOST_THREAD_LIB}
        Threads::Threads
        nlohmann_json::nlohmann_json
		zmq
)

# -----------------------------
# Debug info
# -----------------------------
message(STATUS "Boost include dir: ${BOOST_INCLUDE_DIR}")
message(STATUS "Boost system lib: ${BOOST_SYSTEM_LIB}")
message(STATUS "Boost thread lib: ${BOOST_THREAD_LIB}")
