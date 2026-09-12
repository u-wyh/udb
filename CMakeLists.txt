cmake_minimum_required(VERSION 3.22)

project(udb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(udb_storage src/disk_manager.cpp src/buffer_pool_manager.cpp src/slotted_page.cpp src/table_heap.cpp src/b_plus_tree.cpp)
target_include_directories(udb_storage PUBLIC include)

add_library(udb_types src/tuple.cpp)
target_include_directories(udb_types PUBLIC include)

add_library(udb_catalog src/catalog.cpp src/database.cpp)
target_link_libraries(udb_catalog PUBLIC udb_storage udb_types)

add_library(udb_sql src/sql/lexer.cpp src/sql/parser.cpp)
target_include_directories(udb_sql PUBLIC include)

add_library(udb_binder src/sql/binder.cpp)
target_link_libraries(udb_binder PUBLIC udb_sql udb_catalog)

add_library(udb_planner src/sql/planner.cpp)
target_link_libraries(udb_planner PUBLIC udb_catalog)

add_library(udb_executor src/sql/executor.cpp src/sql/expression.cpp)
target_link_libraries(udb_executor PUBLIC udb_catalog udb_planner)

add_library(udb_sql_engine src/sql/engine.cpp)
target_link_libraries(udb_sql_engine PUBLIC udb_executor udb_binder)

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

add_executable(udb_database_test tests/database_test.cpp)
target_link_libraries(udb_database_test PRIVATE udb_catalog)
add_test(NAME database COMMAND udb_database_test)

add_executable(udb_sql_test tests/sql_test.cpp)
target_link_libraries(udb_sql_test PRIVATE udb_sql)
add_test(NAME sql COMMAND udb_sql_test)

add_executable(udb_binder_test tests/binder_test.cpp)
target_link_libraries(udb_binder_test PRIVATE udb_binder)
add_test(NAME binder COMMAND udb_binder_test)

add_executable(udb_planner_test tests/planner_test.cpp)
target_link_libraries(udb_planner_test PRIVATE udb_planner udb_binder)
add_test(NAME planner COMMAND udb_planner_test)

add_executable(udb_executor_test tests/executor_test.cpp)
target_link_libraries(udb_executor_test PRIVATE udb_executor udb_binder)
add_test(NAME executor COMMAND udb_executor_test)

add_executable(udb_sql_engine_test tests/sql_engine_test.cpp)
target_link_libraries(udb_sql_engine_test PRIVATE udb_sql_engine)
add_test(NAME sql_engine COMMAND udb_sql_engine_test)

add_executable(udb_expression_test tests/expression_test.cpp)
target_link_libraries(udb_expression_test PRIVATE udb_sql_engine)
add_test(NAME expression COMMAND udb_expression_test)

add_executable(udb_delete_test tests/delete_test.cpp)
target_link_libraries(udb_delete_test PRIVATE udb_sql_engine)
add_test(NAME delete COMMAND udb_delete_test)

add_executable(udb_update_test tests/update_test.cpp)
target_link_libraries(udb_update_test PRIVATE udb_sql_engine)
add_test(NAME update COMMAND udb_update_test)

add_executable(udb_drop_table_test tests/drop_table_test.cpp)
target_link_libraries(udb_drop_table_test PRIVATE udb_sql_engine)
add_test(NAME drop_table COMMAND udb_drop_table_test)

add_executable(udb_free_page_test tests/free_page_test.cpp)
target_link_libraries(udb_free_page_test PRIVATE udb_sql_engine)
add_test(NAME free_page COMMAND udb_free_page_test)

add_executable(udb_b_plus_tree_test tests/b_plus_tree_test.cpp)
target_link_libraries(udb_b_plus_tree_test PRIVATE udb_storage)
add_test(NAME b_plus_tree COMMAND udb_b_plus_tree_test)

add_executable(udb_create_index_test tests/create_index_test.cpp)
target_link_libraries(udb_create_index_test PRIVATE udb_sql_engine)
add_test(NAME create_index COMMAND udb_create_index_test)

add_executable(udb_index_maintenance_test tests/index_maintenance_test.cpp)
target_link_libraries(udb_index_maintenance_test PRIVATE udb_sql_engine)
add_test(NAME index_maintenance COMMAND udb_index_maintenance_test)

add_executable(udb_index_scan_test tests/index_scan_test.cpp)
target_link_libraries(udb_index_scan_test PRIVATE udb_sql_engine)
add_test(NAME index_scan COMMAND udb_index_scan_test)

add_executable(udb_range_scan_test tests/range_scan_test.cpp)
target_link_libraries(udb_range_scan_test PRIVATE udb_sql_engine)
add_test(NAME range_scan COMMAND udb_range_scan_test)

add_executable(udb_drop_index_test tests/drop_index_test.cpp)
target_link_libraries(udb_drop_index_test PRIVATE udb_sql_engine)
add_test(NAME drop_index COMMAND udb_drop_index_test)

add_executable(udb_limit_test tests/limit_test.cpp)
target_link_libraries(udb_limit_test PRIVATE udb_sql_engine)
add_test(NAME limit COMMAND udb_limit_test)

add_executable(udb_order_by_test tests/order_by_test.cpp)
target_link_libraries(udb_order_by_test PRIVATE udb_sql_engine)
add_test(NAME order_by COMMAND udb_order_by_test)

add_executable(udb_multi_order_by_test tests/multi_order_by_test.cpp)
target_link_libraries(udb_multi_order_by_test PRIVATE udb_sql_engine)
add_test(NAME multi_order_by COMMAND udb_multi_order_by_test)

add_executable(udb_offset_test tests/offset_test.cpp)
target_link_libraries(udb_offset_test PRIVATE udb_sql_engine)
add_test(NAME offset COMMAND udb_offset_test)

add_executable(udb_aggregate_test tests/aggregate_test.cpp)
target_link_libraries(udb_aggregate_test PRIVATE udb_sql_engine)
add_test(NAME aggregate COMMAND udb_aggregate_test)

add_executable(udb_group_by_test tests/group_by_test.cpp)
target_link_libraries(udb_group_by_test PRIVATE udb_sql_engine)
add_test(NAME group_by COMMAND udb_group_by_test)

add_executable(udb_having_test tests/having_test.cpp)
target_link_libraries(udb_having_test PRIVATE udb_sql_engine)
add_test(NAME having COMMAND udb_having_test)

add_executable(udb_select_expression_test tests/select_expression_test.cpp)
target_link_libraries(udb_select_expression_test PRIVATE udb_sql_engine)
add_test(NAME select_expression COMMAND udb_select_expression_test)

add_executable(udb_cross_join_test tests/cross_join_test.cpp)
target_link_libraries(udb_cross_join_test PRIVATE udb_sql_engine)
add_test(NAME cross_join COMMAND udb_cross_join_test)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target udb udb_storage udb_storage_test udb_buffer_pool_test udb_slotted_page_test udb_table_heap_test udb_types udb_tuple_test udb_catalog udb_catalog_test udb_database_test udb_sql udb_sql_test udb_binder udb_binder_test udb_planner udb_planner_test udb_executor udb_executor_test udb_sql_engine udb_sql_engine_test udb_expression_test udb_delete_test udb_update_test udb_drop_table_test udb_free_page_test udb_b_plus_tree_test udb_create_index_test udb_index_maintenance_test udb_index_scan_test udb_range_scan_test udb_drop_index_test udb_limit_test udb_order_by_test udb_multi_order_by_test udb_offset_test udb_aggregate_test udb_group_by_test udb_having_test udb_select_expression_test udb_cross_join_test)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()
