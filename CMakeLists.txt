cmake_minimum_required(VERSION 3.24)

project(order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORDERBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ORDERBOOK_ENABLE_ASAN "Enable address sanitizer for supported compilers" OFF)

add_library(orderbook
    src/order_book.cpp
    src/jsonl.cpp
    src/synthetic.cpp
)

target_include_directories(orderbook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(orderbook PRIVATE
    -Wall -Wextra -Wpedantic
)

if(ORDERBOOK_WARNINGS_AS_ERRORS)
    target_compile_options(orderbook PRIVATE -Werror)
endif()

if(ORDERBOOK_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
    target_compile_options(orderbook PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(orderbook PUBLIC -fsanitize=address,undefined)
endif()

add_executable(orderbook_demo apps/orderbook_demo.cpp)
target_link_libraries(orderbook_demo PRIVATE orderbook)
target_compile_options(orderbook_demo PRIVATE -Wall -Wextra -Wpedantic)

add_executable(orderbook_bench bench/orderbook_bench.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook)
target_compile_options(orderbook_bench PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(orderbook_tests tests/orderbook_tests.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook)
target_compile_options(orderbook_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME orderbook_tests COMMAND orderbook_tests)

add_executable(orderbook_parser_fuzz tests/parser_fuzz.cpp)
target_link_libraries(orderbook_parser_fuzz PRIVATE orderbook)
target_compile_options(orderbook_parser_fuzz PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME orderbook_parser_fuzz COMMAND orderbook_parser_fuzz)
