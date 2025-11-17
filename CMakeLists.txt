cmake_minimum_required(VERSION 3.10)
project(ups_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# include directory for headers
include_directories(src)

# gather all .cpp sources from src/
file(GLOB SRC_FILES "src/*.cpp")

add_executable(ups_server ${SRC_FILES})
