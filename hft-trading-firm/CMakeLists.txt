cmake_minimum_required(VERSION 3.16)

project(HFTTradingFirm)

# Enforce C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Aggressive compiler optimizations for the Hot Path
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address")

# Threading library
find_package(Threads REQUIRED)

# Include Directories
include_directories(src)
include_directories(include)
# The wire schema is owned by the engine and included from there — there is no
# second copy in this project (see hft_engine/src/protocol/messages.h).
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../hft_engine/src)

# Fetch simdjson
include(FetchContent)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG v3.6.4 # Use latest stable version
)
FetchContent_MakeAvailable(simdjson)



find_package(OpenSSL REQUIRED)

# Main Executable for the Trading Firm
add_executable(trading_firm src/main.cpp)

target_compile_options(trading_firm PRIVATE -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(trading_firm PRIVATE 
    Threads::Threads 
    simdjson::simdjson 
    curl 
    OpenSSL::SSL 
    OpenSSL::Crypto
)
