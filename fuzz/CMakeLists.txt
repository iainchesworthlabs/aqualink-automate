# fuzz/CMakeLists.txt — protocol-decoder fuzz harnesses (opt-in: ENABLE_FUZZING)
#
# Added from the top-level CMakeLists only when ENABLE_FUZZING is ON.  The protocol
# libraries (libaqualink-jandy / libaqualink-pentair / libaqualink-automate) are
# instrumented for fuzzing in the top-level CMakeLists via
# target_enable_fuzzing_instrumentation(); here we build one executable per
# harness and apply the fuzzer engine flags via target_enable_fuzzer().
#
# Engine (AQ_FUZZING_ENGINE, set by cmake/Fuzzing.cmake):
#   * libfuzzer  — Clang -fsanitize=fuzzer supplies main(); coverage-guided.
#   * standalone — fuzz/standalone_main.cpp supplies main(); replays a corpus.
#
# Each harness target <name> is built from the source <name-with-underscores>.cpp,
# e.g. fuzz-jandy-message -> fuzz_jandy_message.cpp.

set(_aq_fuzz_harnesses
    fuzz-jandy-message
    fuzz-pentair-message
)

foreach(_target IN LISTS _aq_fuzz_harnesses)
    string(REPLACE "-" "_" _source "${_target}")
    set(_source "${_source}.cpp")

    add_executable(${_target})

    target_sources(${_target} PRIVATE ${_source})

    # The standalone engine has no libFuzzer runtime, so supply our own main().
    if(AQ_FUZZING_ENGINE STREQUAL "standalone")
        target_sources(${_target} PRIVATE standalone_main.cpp)
    endif()

    # fuzz_frame_builders.h lives alongside the harness sources.
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    target_link_libraries(${_target}
        PRIVATE
        libaqualink-automate
        libaqualink-jandy
        libaqualink-pentair
    )

    target_enable_fuzzer(${_target})
endforeach()

# Convenience aggregate target: build every harness with `cmake --build ... --target fuzzers`.
add_custom_target(fuzzers DEPENDS ${_aq_fuzz_harnesses})

# ---------------------------------------------------------------------------
# CTest integration
# ---------------------------------------------------------------------------
# Make the fuzz harnesses part of `ctest` under a fuzzing preset, so a fuzzing
# test run actually exercises them:
#   * libfuzzer engine  -> a short, bounded coverage-guided run per harness.
#   * standalone engine -> replay the seeded corpus through each harness.
# NOTE: the ALWAYS-ON regression gate for RS-485 decode changes is the ordinary
# unit test test/unit/fuzz/test_protocol_fuzz_smoke.cpp, which runs in the normal
# `testaqualink-automate` suite on EVERY build (no Clang / fuzzing preset needed).
# These CTest entries are the deeper, opt-in libFuzzer layer.
set(AQ_FUZZ_CTEST_SECONDS "30" CACHE STRING "Seconds each harness runs under `ctest` (libFuzzer engine).")

find_package(Python3 COMPONENTS Interpreter QUIET)

# Seed the corpus from the capture fixtures as a CTest setup fixture, so the
# per-harness tests below start from real traffic. Skipped if Python is absent
# (a libFuzzer run still fuzzes from scratch; a standalone replay just no-ops).
if(Python3_FOUND)
    add_test(
        NAME fuzz-seed-corpus
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/seed_corpus.py")
    set_tests_properties(fuzz-seed-corpus PROPERTIES FIXTURES_SETUP fuzz_corpus)
endif()

foreach(_target IN LISTS _aq_fuzz_harnesses)
    string(REGEX REPLACE "^fuzz-([a-z0-9]+)-message$" "\\1" _proto "${_target}")
    set(_corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${_proto}")
    file(MAKE_DIRECTORY "${_corpus}") # libFuzzer requires the corpus dir to exist.

    if(AQ_FUZZING_ENGINE STREQUAL "libfuzzer")
        add_test(
            NAME ${_target}-fuzz
            COMMAND ${_target} "${_corpus}"
                    -max_total_time=${AQ_FUZZ_CTEST_SECONDS}
                    -rss_limit_mb=4096 -timeout=25 -print_final_stats=1)
        # Give the process headroom over its own -max_total_time budget.
        math(EXPR _ctest_timeout "${AQ_FUZZ_CTEST_SECONDS} + 60")
        set_tests_properties(${_target}-fuzz PROPERTIES
            FIXTURES_REQUIRED fuzz_corpus
            TIMEOUT ${_ctest_timeout}
            LABELS "fuzz")
    else()
        add_test(NAME ${_target}-replay COMMAND ${_target} "${_corpus}")
        set_tests_properties(${_target}-replay PROPERTIES
            FIXTURES_REQUIRED fuzz_corpus
            LABELS "fuzz")
    endif()
endforeach()
