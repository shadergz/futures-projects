cmake_minimum_required(VERSION 3.23)
project(httpserver)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(Threads REQUIRED)

add_executable(httpserver main.cpp)
target_link_libraries(httpserver Threads::Threads)
