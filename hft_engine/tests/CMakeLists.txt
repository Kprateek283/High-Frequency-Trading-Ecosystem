add_executable(engine_tests
    main.cpp
    test_wire.cpp
    test_symbol.cpp
    test_queue.cpp
    test_orderbook.cpp
    test_framing.cpp
)

target_compile_options(engine_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

# The Release flags carry -DNDEBUG, which turns assert() into a no-op and would
# make the whole suite pass vacuously. Tests keep the real optimizer flags (they
# exercise the same codegen production runs) but with NDEBUG stripped back out.
target_compile_options(engine_tests PRIVATE -UNDEBUG)

target_link_libraries(engine_tests PRIVATE Threads::Threads)

add_test(NAME engine_tests COMMAND engine_tests)
