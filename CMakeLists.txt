cmake_minimum_required(VERSION 3.16)
project(sfu_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(src/external/libdatachannel)

find_package(jwt-cpp CONFIG REQUIRED)

add_executable(sfu_server src/main.cpp src/room.cpp src/router.cpp src/loop.cpp src/participant.cpp src/utils.cpp)

target_include_directories(sfu_server PRIVATE libdatachannel/include)

target_link_libraries(sfu_server
  PRIVATE
    datachannel
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
)
