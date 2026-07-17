cmake_minimum_required(VERSION 3.16)
project(TradingEcosystem)

# One known path for every binary, so scripts and the Python orchestrator
# do not have to know which subproject produced what.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

add_subdirectory(hft_engine)
add_subdirectory(hft-trading-firm)
