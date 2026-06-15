cmake_minimum_required(VERSION 3.15)

project(MC_DEV_TOOL_TEST LANGUAGES CXX C)

add_executable(test1 test1.cpp)
target_compile_features(test1 PRIVATE cxx_std_20)


add_executable(test2 test2.cpp)
target_compile_features(test2 PRIVATE cxx_std_23)
target_link_libraries(test2 PRIVATE mcdevtool)

add_executable(test3 test3.cpp)
target_compile_features(test3 PRIVATE cxx_std_23)

add_executable(mcp_server_test mcp_server_test.cpp)
target_compile_features(mcp_server_test PRIVATE cxx_std_23)
target_link_libraries(mcp_server_test PRIVATE mcdevtool mcp)

add_executable(captureTest captureTest.cpp)
target_compile_features(captureTest PRIVATE cxx_std_23)
target_link_libraries(captureTest PRIVATE mcdevtool)

# GameLogProcessor 单元测试(纯头文件，无需链接 mcdevtool)。
add_executable(game_log_processor_test game_log_processor_test.cpp)
target_compile_features(game_log_processor_test PRIVATE cxx_std_23)
target_include_directories(game_log_processor_test PRIVATE ${CMAKE_SOURCE_DIR}/tools/mcdk)