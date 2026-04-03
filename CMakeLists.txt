cmake_minimum_required(VERSION 3.16)
project(FlexQL LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options($<$<CONFIG:Release>:-O3;-DNDEBUG;-flto;-march=native>)
add_link_options($<$<CONFIG:Release>:-flto>)

include_directories(include src)
include_directories(src/client src/server src/network src/query tests)

add_library(flexql_client STATIC
    src/client/flexql_client.cpp
    src/network/protocol.cpp
)

target_include_directories(flexql_client PUBLIC include)

target_compile_options(flexql_client PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flexql_server
    src/server/server_main.cpp
    src/query/sql_engine.cpp
    src/network/protocol.cpp
)

target_compile_options(flexql_server PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(flexql_server PRIVATE pthread)

add_executable(flexql_client_cli
    src/client/repl_main.cpp
)

set_target_properties(flexql_client_cli PROPERTIES OUTPUT_NAME "flexql-client")

target_link_libraries(flexql_client_cli PRIVATE flexql_client)

target_compile_options(flexql_client_cli PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flexql_repl
    src/client/repl_main.cpp
)

target_link_libraries(flexql_repl PRIVATE flexql_client)

target_compile_options(flexql_repl PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flexql_smoke_test
    tests/smoke_test.cpp
)

target_link_libraries(flexql_smoke_test PRIVATE flexql_client)

target_compile_options(flexql_smoke_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flexql_benchmark
    tests/benchmark_flexql.cpp
)

target_link_libraries(flexql_benchmark PRIVATE flexql_client)

target_compile_options(flexql_benchmark PRIVATE -Wall -Wextra -Wpedantic)
