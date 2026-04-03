cmake_minimum_required(VERSION 3.20)
project(orderbook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(orderbook
    main.cpp
    Core_Infra/orderbook.cpp
    Core_Infra/ring_buffer.cpp
    Core_Infra/matching_engine.cpp
    Core_Infra/gateway.cpp
)

target_include_directories(orderbook PRIVATE Core_Infra)

find_package(Threads REQUIRED)
target_link_libraries(orderbook Threads::Threads)
