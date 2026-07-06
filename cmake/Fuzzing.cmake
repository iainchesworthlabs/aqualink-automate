# Fuzzing.cmake — libFuzzer harness configuration for the protocol decoders
#
# Provides:
#   target_enable_fuzzing_instrumentation(<target>)  — for the libraries UNDER TEST
#   target_enable_fuzzer(<target>)                   — for a fuzz-harness executable
#
# and sets the cache-scoped variable:
#   AQ_FUZZING_ENGINE   — "libfuzzer" or "standalone"
#
# Engine selection
# ----------------
# Coverage-guided fuzzing uses LLVM libFuzzer (-fsanitize=fuzzer), which is a
# Clang-only feature.  We enable it on non-MSVC Clang (Linux / macOS — the same
# environment OSS-Fuzz builds in).  On every other compiler (MSVC cl.exe, and
# clang-cl, whose Windows libFuzzer support is finicky) we fall back to the
# "standalone" engine: the harness is linked with fuzz/standalone_main.cpp, which
# replays a corpus of inputs through LLVMFuzzerTestOneInput without mutation.  That
# keeps the harnesses buildable and the corpus replayable everywhere (crash repro,
# CI smoke test) even without a fuzzing engine.
#
# Platform-isolation note: this module carries the compiler-conditional fuzzer
# flags so that src/ CMakeLists stays free of $<CXX_COMPILER_ID> conditionals, in
# line with cmake/Sanitizers.cmake and the project's platform-isolation policy.

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    set(AQ_FUZZING_ENGINE "libfuzzer" CACHE INTERNAL "Selected fuzzing engine")
else()
    set(AQ_FUZZING_ENGINE "standalone" CACHE INTERNAL "Selected fuzzing engine")
endif()

message(STATUS "Fuzzing: engine = ${AQ_FUZZING_ENGINE} (compiler: ${CMAKE_CXX_COMPILER_ID})")

# Compile the code UNDER TEST (the protocol-decode libraries) with coverage
# instrumentation + AddressSanitizer so the fuzzer can guide mutation and detect
# out-of-bounds reads.  -fsanitize=fuzzer-no-link adds SanitizerCoverage without
# pulling in libFuzzer's main(); the harness executable provides that.
function(target_enable_fuzzing_instrumentation TARGET)
    if(NOT ENABLE_FUZZING)
        return()
    endif()

    get_target_property(_type ${TARGET} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    if(AQ_FUZZING_ENGINE STREQUAL "libfuzzer")
        target_compile_options(${TARGET} PUBLIC
            -fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer -g)
        target_link_options(${TARGET} PUBLIC -fsanitize=address)
    endif()
    # standalone engine: no instrumentation — plain replay of the corpus.
endfunction()

# Compile + link a fuzz-harness executable.  In libFuzzer mode this pulls in the
# libFuzzer runtime (which supplies main()) alongside ASan; in standalone mode the
# harness must additionally be linked with fuzz/standalone_main.cpp (the caller
# adds it based on AQ_FUZZING_ENGINE).
function(target_enable_fuzzer TARGET)
    if(NOT ENABLE_FUZZING)
        return()
    endif()

    if(AQ_FUZZING_ENGINE STREQUAL "libfuzzer")
        target_compile_options(${TARGET} PRIVATE
            -fsanitize=fuzzer,address -fno-omit-frame-pointer -g)
        target_link_options(${TARGET} PRIVATE -fsanitize=fuzzer,address)
    endif()
endfunction()
