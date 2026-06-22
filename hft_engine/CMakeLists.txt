cmake_minimum_required(VERSION 3.16)

project(HFTEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(src)

add_executable(exchange src/app/exchange.cpp src/matching/engine.cpp)
add_executable(tester src/tools/tester.cpp)
add_executable(market_maker src/tools/market_maker.cpp)
add_executable(generate_pcap src/tools/generate_pcap.cpp)
add_executable(replay src/tools/replay.cpp)
