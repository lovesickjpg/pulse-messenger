cmake_minimum_required(VERSION 3.24)

project(pulse_messenger VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS asio)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(Threads REQUIRED)
include(CTest)

add_library(pulse_protocol src/protocol/protocol.cpp)
target_include_directories(pulse_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pulse_protocol PUBLIC nlohmann_json::nlohmann_json)

add_executable(pulse-server apps/server/main.cpp)
add_executable(pulse-client apps/client-cli/main.cpp)
add_library(pulse_transport src/transport/connection.cpp)
target_include_directories(pulse_transport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pulse_transport PUBLIC pulse_protocol Boost::asio Threads::Threads)
if(WIN32)
    target_compile_definitions(pulse_transport PUBLIC _WIN32_WINNT=0x0A00)
endif()

# The SQL in migrations/ gets compiled into the library, so a binary always
# knows its own schema and there's no file path to get wrong at runtime.
file(GLOB pulse_migration_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/migrations/*.sql")
list(SORT pulse_migration_files)
if(NOT pulse_migration_files)
    message(FATAL_ERROR "No migrations found in ${CMAKE_CURRENT_SOURCE_DIR}/migrations")
endif()
set(PULSE_MIGRATION_ENTRIES "")
foreach(migration_file IN LISTS pulse_migration_files)
    get_filename_component(migration_stem "${migration_file}" NAME_WE)
    if(NOT migration_stem MATCHES "^([0-9]+)_(.+)$")
        message(FATAL_ERROR "Migration ${migration_file} must be named NNN_description.sql")
    endif()
    # strip leading zeros so 008 isn't read as octal
    string(REGEX REPLACE "^0+([0-9])" "\\1" migration_version "${CMAKE_MATCH_1}")
    file(READ "${migration_file}" migration_sql)
    string(APPEND PULSE_MIGRATION_ENTRIES
        "    {${migration_version}, \"${migration_stem}\", R\"pulse_sql(${migration_sql})pulse_sql\"},
")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${migration_file}")
endforeach()
configure_file(src/storage/migrations.cpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/migrations.cpp" @ONLY)

add_library(pulse_storage
    src/storage/database.cpp
    src/storage/database_worker.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/migrations.cpp")
target_include_directories(pulse_storage
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/storage")
target_link_libraries(pulse_storage PUBLIC Boost::asio Threads::Threads PRIVATE SQLiteCpp)
if(WIN32)
    target_compile_definitions(pulse_storage PUBLIC _WIN32_WINNT=0x0A00)
endif()

target_link_libraries(pulse-server PRIVATE pulse_transport pulse_storage)
target_link_libraries(pulse-client PRIVATE pulse_transport)

if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    add_executable(pulse-protocol-tests tests/protocol/protocol_tests.cpp)
    target_link_libraries(pulse-protocol-tests PRIVATE pulse_protocol Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(pulse-protocol-tests)
    add_executable(pulse-storage-tests tests/storage/storage_tests.cpp)
    target_link_libraries(pulse-storage-tests PRIVATE pulse_storage SQLiteCpp Catch2::Catch2WithMain)
    catch_discover_tests(pulse-storage-tests)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME transport-integration
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/transport/integration.py"
                "$<TARGET_FILE:pulse-server>" "$<TARGET_FILE:pulse-client>")
    set_tests_properties(transport-integration PROPERTIES TIMEOUT 60)
endif()

set(pulse_targets pulse-server pulse-client pulse_protocol pulse_transport pulse_storage)
if(BUILD_TESTING)
    list(APPEND pulse_targets pulse-protocol-tests pulse-storage-tests)
endif()
foreach(target IN LISTS pulse_targets)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
