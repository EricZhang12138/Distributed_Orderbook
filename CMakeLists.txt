cmake_minimum_required(VERSION 3.20)
project(orderbook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# shared library of all core infra, so both the main binary and the tests can link to it
add_library(orderbook_core
    Core_Infra/orderbook.cpp
    Core_Infra/ring_buffer.cpp
    Core_Infra/matching_engine.cpp
    Core_Infra/gateway.cpp
)
target_include_directories(orderbook_core PUBLIC Core_Infra)

find_package(Threads REQUIRED)
target_link_libraries(orderbook_core PUBLIC Threads::Threads)

add_executable(orderbook main.cpp)
target_link_libraries(orderbook PRIVATE orderbook_core)

# ---------- tests ----------
enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# prevent gtest from overriding our compiler/linker options on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(orderbook_tests
    tests/orderbook_test.cpp
    tests/ring_buffer_test.cpp
    tests/gateway_test.cpp
    tests/end_to_end_test.cpp
)
target_link_libraries(orderbook_tests PRIVATE orderbook_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(orderbook_tests)
