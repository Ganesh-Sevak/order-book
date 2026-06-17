cmake_minimum_required(VERSION 3.24)

project(order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORDERBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ORDERBOOK_ENABLE_ASAN "Enable address sanitizer for supported compilers" OFF)
option(ORDERBOOK_ENABLE_LIBFUZZER "Build parser fuzz target with LibFuzzer" OFF)

add_library(orderbook_warnings INTERFACE)
target_compile_options(orderbook_warnings INTERFACE
    -Wall -Wextra -Wpedantic
)

if(ORDERBOOK_WARNINGS_AS_ERRORS)
    target_compile_options(orderbook_warnings INTERFACE -Werror)
endif()

add_library(orderbook
    src/order_book.cpp
    src/jsonl.cpp
    src/synthetic.cpp
)

target_include_directories(orderbook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(orderbook PRIVATE orderbook_warnings)

if(ORDERBOOK_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
    target_compile_options(orderbook PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(orderbook PUBLIC -fsanitize=address,undefined)
endif()

add_executable(orderbook_demo apps/orderbook_demo.cpp)
target_link_libraries(orderbook_demo PRIVATE orderbook orderbook_warnings)

add_executable(orderbook_bench bench/orderbook_bench.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook orderbook_warnings)

enable_testing()

add_executable(orderbook_tests tests/orderbook_tests.cpp)
target_include_directories(orderbook_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(orderbook_tests PRIVATE orderbook orderbook_warnings)
add_test(NAME orderbook_tests COMMAND orderbook_tests)

add_executable(orderbook_parser_corpus tests/parser_corpus.cpp)
target_link_libraries(orderbook_parser_corpus PRIVATE orderbook orderbook_warnings)
add_test(NAME orderbook_parser_corpus COMMAND orderbook_parser_corpus)

if(ORDERBOOK_ENABLE_LIBFUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ORDERBOOK_ENABLE_LIBFUZZER requires Clang")
    endif()
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" ORDERBOOK_HAS_LIBFUZZER_RUNTIME)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT ORDERBOOK_HAS_LIBFUZZER_RUNTIME)
        message(FATAL_ERROR "ORDERBOOK_ENABLE_LIBFUZZER was requested, but this Clang toolchain cannot link the LibFuzzer runtime")
    endif()
    add_executable(orderbook_parser_fuzzer tests/parser_libfuzzer.cpp)
    target_link_libraries(orderbook_parser_fuzzer PRIVATE orderbook orderbook_warnings)
    target_compile_options(orderbook_parser_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(orderbook_parser_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
