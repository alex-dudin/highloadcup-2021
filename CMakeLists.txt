cmake_minimum_required(VERSION 3.17)

project(goldrush-client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS thread)
find_package(c-ares REQUIRED)
find_package(cpuid REQUIRED)
find_package(CLI11 REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(spdlog REQUIRED)

add_executable(${PROJECT_NAME} client.cpp ares.hpp go_rng_seed_recovery.cpp go_rng_seed_recovery.hpp hardcode.cpp hardcode.hpp serialization.hpp types.hpp)
target_link_libraries(${PROJECT_NAME} ${Boost_LIBRARIES} CLI11::CLI11 cpuid::cpuid c-ares::cares spdlog::spdlog)

target_include_directories(${PROJECT_NAME} PRIVATE ${Boost_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_DIRS})
target_compile_definitions(${PROJECT_NAME} PRIVATE BOOST_BEAST_USE_STD_STRING_VIEW)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0A00)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Ofast)
endif()