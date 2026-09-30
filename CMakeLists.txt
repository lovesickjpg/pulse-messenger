cmake_minimum_required(VERSION 3.24)

project(pulse_messenger VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(nlohmann_json CONFIG REQUIRED)
include(CTest)

add_library(pulse_protocol src/protocol/protocol.cpp)
target_include_directories(pulse_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pulse_protocol PUBLIC nlohmann_json::nlohmann_json)

add_executable(pulse-server apps/server/main.cpp)
add_executable(pulse-client apps/client-cli/main.cpp)

if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    add_executable(pulse-protocol-tests tests/protocol/protocol_tests.cpp)
    target_link_libraries(pulse-protocol-tests PRIVATE pulse_protocol Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(pulse-protocol-tests)
endif()

set(pulse_targets pulse-server pulse-client pulse_protocol)
if(BUILD_TESTING)
    list(APPEND pulse_targets pulse-protocol-tests)
endif()
foreach(target IN LISTS pulse_targets)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
