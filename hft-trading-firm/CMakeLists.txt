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

# book_builder.h includes <boost/container/flat_map.hpp>. Header-only, but it was
# previously undeclared -- the build simply assumed Boost sat on the default
# include path, so it compiled on developer machines and died mid-compile with a
# bare "No such file or directory" anywhere else. Declaring it fails at configure
# time with an actionable message instead.
find_package(Boost REQUIRED)

# Include Directories
include_directories(src)
include_directories(include)
# The wire schema is owned by the engine and included from there — there is no
# second copy in this project (see hft_engine/src/protocol/messages.h).
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../hft_engine/src)

# The Binance CryptoPaperConnector is the ONLY thing that needs simdjson/curl/OpenSSL,
# and its pinned simdjson (v3.6.4) does not build on newer GCC. It is off by default
# so the local firm builds with just Threads + header-only Boost (book_builder's
# flat_map). Enable with -DWITH_CRYPTO=ON — that path additionally needs a simdjson
# version compatible with your GCC.
option(WITH_CRYPTO "Build the Binance CryptoPaperConnector (needs simdjson/curl/OpenSSL)" OFF)

# Main Executable for the Trading Firm
add_executable(trading_firm src/main.cpp)

target_compile_options(trading_firm PRIVATE -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(trading_firm PRIVATE Threads::Threads Boost::headers)

if(WITH_CRYPTO)
    include(FetchContent)
    FetchContent_Declare(
      simdjson
      GIT_REPOSITORY https://github.com/simdjson/simdjson.git
      GIT_TAG v3.6.4 # Use latest stable version
    )
    FetchContent_MakeAvailable(simdjson)
    find_package(OpenSSL REQUIRED)

    target_compile_definitions(trading_firm PRIVATE WITH_CRYPTO)
    # Treat simdjson headers as system headers so their warnings don't trip -Werror.
    target_include_directories(trading_firm SYSTEM PRIVATE ${simdjson_SOURCE_DIR}/include)
    target_link_libraries(trading_firm PRIVATE
        simdjson::simdjson
        curl
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()
