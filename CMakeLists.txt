cmake_minimum_required(VERSION 3.22)

project(udb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(udb_storage src/disk_manager.cpp src/buffer_pool_manager.cpp src/slotted_page.cpp src/table_heap.cpp)
target_include_directories(udb_storage PUBLIC include)

add_library(udb_types src/tuple.cpp)
target_include_directories(udb_types PUBLIC include)

add_library(udb_catalog src/catalog.cpp)
target_link_libraries(udb_catalog PUBLIC udb_storage udb_types)

add_executable(udb src/main.cpp)
target_link_libraries(udb PRIVATE udb_storage)

enable_testing()
add_executable(udb_storage_test tests/storage_test.cpp)
target_link_libraries(udb_storage_test PRIVATE udb_storage)
add_test(NAME storage COMMAND udb_storage_test)

add_executable(udb_buffer_pool_test tests/buffer_pool_test.cpp)
target_link_libraries(udb_buffer_pool_test PRIVATE udb_storage)
add_test(NAME buffer_pool COMMAND udb_buffer_pool_test)

add_executable(udb_slotted_page_test tests/slotted_page_test.cpp)
target_link_libraries(udb_slotted_page_test PRIVATE udb_storage)
add_test(NAME slotted_page COMMAND udb_slotted_page_test)

add_executable(udb_table_heap_test tests/table_heap_test.cpp)
target_link_libraries(udb_table_heap_test PRIVATE udb_storage)
add_test(NAME table_heap COMMAND udb_table_heap_test)

add_executable(udb_tuple_test tests/tuple_test.cpp)
target_link_libraries(udb_tuple_test PRIVATE udb_types udb_storage)
add_test(NAME tuple COMMAND udb_tuple_test)

add_executable(udb_catalog_test tests/catalog_test.cpp)
target_link_libraries(udb_catalog_test PRIVATE udb_catalog)
add_test(NAME catalog COMMAND udb_catalog_test)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target udb udb_storage udb_storage_test udb_buffer_pool_test udb_slotted_page_test udb_table_heap_test udb_types udb_tuple_test udb_catalog udb_catalog_test)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()
