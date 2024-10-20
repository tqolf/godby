cmake_minimum_required(VERSION 3.15)
project(god-bless-you)

enable_testing()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/CC.cmake)
include(cmake/CPM.cmake)

add_subdirectory(godby)
add_subdirectory(samples)
