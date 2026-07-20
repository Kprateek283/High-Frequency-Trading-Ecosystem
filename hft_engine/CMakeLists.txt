cmake_minimum_required(VERSION 3.16)

project(HFTEngine)

# Enforce C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Aggressive compiler optimizations for the Hot Path (same as hft-trading-firm)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address")

# Threading library
find_package(Threads REQUIRED)

# Include Directories
include_directories(src)

add_executable(exchange src/app/exchange.cpp src/matching/engine.cpp)
add_executable(tester src/tools/tester.cpp)
add_executable(market_maker src/tools/market_maker.cpp)
add_executable(generate_pcap src/tools/generate_pcap.cpp)
add_executable(replay src/tools/replay.cpp)
add_executable(liquidity src/tools/liquidity.cpp)

foreach(target exchange tester market_maker generate_pcap replay liquidity)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wpedantic)
    target_link_libraries(${target} PRIVATE Threads::Threads)
endforeach()

add_subdirectory(tests)
