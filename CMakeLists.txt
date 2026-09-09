cmake_minimum_required(VERSION 3.22)

project(udb LANGUAGES CXX)

add_executable(udb src/main.cpp)

set_target_properties(udb PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(udb PRIVATE -Wall -Wextra -Wpedantic)
endif()
