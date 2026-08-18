cmake_minimum_required (VERSION 3.31 FATAL_ERROR)

if(POLICY CMP0053)
  cmake_policy(SET CMP0053 NEW)
endif()
if(POLICY CMP0077)
  # Allow preset/command-line variables to override option() defaults
  cmake_policy(SET CMP0077 NEW)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(Boost_NO_WARN_NEW_VERSIONS	 ON) # suppress the warning about unknown dependencies for new Boost versions
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # Project uses headers, not C++20 modules; avoids GCC module flags breaking clang-tidy
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Generate compile_commands.json for clang-tidy
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" "${CMAKE_SOURCE_DIR}/cmake/tools/" "${CMAKE_SOURCE_DIR}/cmake/tools/coverage" "${CMAKE_SOURCE_DIR}/deps")

# Derive version from git tags before project() declaration
include(GitVersionDerivation)

# Define options BEFORE project() to ensure preset values take precedence
# over any vcpkg toolchain cache interference
option(ENABLE_BENCHMARKS "Enable performance tests" ON)
option(ENABLE_COVERAGE "Enable coverage of unit tests" OFF)
option(ENABLE_TESTS "Enable unit tests" ON)
option(ENABLE_PROFILING "DevTools: Enable profiling tooling" ON)
option(ENABLE_CLANG_TIDY "DevTools: Enable static analysis using clang-tidy" OFF)
option(ENABLE_PVS_STUDIO "DevTools: Enable static analysis using PVS-Studio" OFF)
option(ENABLE_ASIO_TRACKING "DevTools: Debug using ASIO connection tracking" OFF)
option(ENABLE_SANITIZERS "DevTools: Enable sanitizer tooling" OFF)
option(ENABLE_FUZZING "DevTools: Build the protocol-decoder libFuzzer harnesses (clang/libFuzzer; standalone replay elsewhere)" OFF)
option(ENABLE_IPO "Enable interprocedural optimisation (LTO) for Release builds when supported" ON)

# ---------------------------------------------------------------------------
# Ensure the vcpkg submodule is checked out BEFORE project() loads the vcpkg
# toolchain, which requires deps/vcpkg/.vcpkg-root. Without this, a fresh clone
# or an IDE "Open Folder" fails configuration with "Could not find .vcpkg-root"
# (and then "CMAKE_C_COMPILER not set") before the compiler is even detected.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/vcpkg/.vcpkg-root")
	find_package(Git QUIET)
	if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
		message(STATUS "vcpkg: deps/vcpkg not initialised; running 'git submodule update --init --recursive deps/vcpkg'")
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" submodule update --init --recursive deps/vcpkg
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			RESULT_VARIABLE AQ_VCPKG_SUBMODULE_RESULT)
		if(NOT AQ_VCPKG_SUBMODULE_RESULT EQUAL 0)
			message(FATAL_ERROR
				"Failed to initialise the vcpkg submodule (deps/vcpkg).\n"
				"Run manually:  git submodule update --init --recursive")
		endif()
	endif()
	if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/vcpkg/.vcpkg-root")
		message(FATAL_ERROR
			"The vcpkg submodule (deps/vcpkg) is not populated -- 'deps/vcpkg/.vcpkg-root' is missing.\n"
			"Run:  git submodule update --init --recursive")
	endif()
endif()

project(
	aqualink-automate
	VERSION ${DERIVED_VERSION_SEMVER}
	DESCRIPTION "Control Jandy Aqualink RS pool equipment via common IoT integrations"
	HOMEPAGE_URL "https://github.com/iainchesworthlabs/aqualink-automate"
	LANGUAGES C CXX)

# Propagate full version info (CMake project() only accepts M.M.P)
set(PROJECT_VERSION_PRERELEASE "${DERIVED_VERSION_PRERELEASE}" CACHE STRING "Prerelease label (e.g. beta.2)" FORCE)
set(PROJECT_VERSION_FULL "${DERIVED_VERSION_FULL}" CACHE STRING "Full semver version (M.M.P[-prerelease])" FORCE)

if(ENABLE_SANITIZERS AND ENABLE_PROFILING)
  message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_PROFILING cannot both be ON. "
    "Profiling instrumentation (Tracy) is incompatible with sanitizer runtimes. "
    "Disable one of them.")
endif()

# The libFuzzer engine builds its targets with its own AddressSanitizer + coverage
# instrumentation, so it must not be combined with the standalone sanitizer or
# profiling instrumentation (double-instrumentation / conflicting runtimes).
if(ENABLE_FUZZING AND ENABLE_PROFILING)
  message(FATAL_ERROR "ENABLE_FUZZING and ENABLE_PROFILING cannot both be ON. "
    "The fuzzer's instrumentation conflicts with profiling runtimes. Disable one of them.")
endif()
if(ENABLE_FUZZING AND ENABLE_SANITIZERS)
  message(FATAL_ERROR "ENABLE_FUZZING and ENABLE_SANITIZERS cannot both be ON. "
    "The libFuzzer engine already enables AddressSanitizer on the fuzz targets. Disable ENABLE_SANITIZERS.")
endif()

option(USE_SONARQUBE "Use SonarQube for coverage of unit tests" OFF)

# ---------------------------------------------------------------------------
# Interprocedural optimisation (LTO/IPO)
# ---------------------------------------------------------------------------
# Detect once whether the active toolchain supports IPO. When supported, it is
# enabled for the Release configuration only (via INTERPROCEDURAL_OPTIMIZATION_
# RELEASE per target after the targets are created) so cross-TU / cross-library
# inlining of the hot byte-scan and checksum paths is available in release
# builds. Debug/coverage/sanitizer builds are intentionally left unoptimised.
# Honour an explicit opt-out via ENABLE_IPO (declared with the other options
# before project() so a preset can override it).

set(AQ_IPO_SUPPORTED OFF)
if(ENABLE_IPO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT AQ_IPO_SUPPORTED OUTPUT AQ_IPO_OUTPUT LANGUAGES C CXX)
	if(AQ_IPO_SUPPORTED)
		message(STATUS "IPO/LTO: supported by the active toolchain; enabled for Release builds")
	else()
		message(STATUS "IPO/LTO: not supported by the active toolchain (${AQ_IPO_OUTPUT}); continuing without it")
	endif()
endif()

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

add_subdirectory(deps)

# benchmark is only consumed by the performance-test target (test/CMakeLists.txt),
# which itself is gated on ENABLE_BENCHMARKS. Only require the package when it can
# actually be linked so a benchmarks-off configure does not fail for a missing dep.
if(ENABLE_BENCHMARKS)
	find_package(benchmark REQUIRED)
endif()

# Boost COMPONENTS list:
#   - graph                : header-only (device_graph / screen_data_page_graph)
#   - iostreams/log/log_setup/program_options/url : compiled libs linked in src/core
#   - regex                : compiled lib (jandy onetouch + string converters)
#   - system               : compiled lib (core serial / errors / http)
#   - unit_test_framework  : compiled lib linked in test/
# regex/system are currently pulled in transitively; the explicit per-target link
# edits live in WU-CORE-CMAKE-CLEANUP (src/core/CMakeLists.txt). This COMPONENTS
# list is the single source of truth for what is requested.
find_package(Boost REQUIRED COMPONENTS graph iostreams log log_setup program_options regex system unit_test_framework url)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(magic_enum REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

if(ENABLE_COVERAGE)

if(MSVC)
  include(OpenCppCoverage)
  # No compiler flags needed - OpenCppCoverage uses PDB debug information
else()
  include(CodeCoverage)
  append_coverage_compiler_flags()
endif()

endif(ENABLE_COVERAGE)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

if(ENABLE_PROFILING)

find_package(Tracy CONFIG REQUIRED)

find_package(UProf)
find_package(VTune)

endif(ENABLE_PROFILING)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

if(ENABLE_SANITIZERS)

option(SANITIZE_ADDRESS "DevTools: Sanitizers -> Use AddressSanitizer" OFF)
option(SANITIZE_MEMORY "DevTools: Sanitizers -> Use MemorySanitizer" OFF)
option(SANITIZE_THREAD "DevTools: Sanitizers -> Use ThreadSanitizer" OFF)
option(SANITIZE_UNDEFINED "DevTools: Sanitizers -> Use UndefinedBehaviourSanitizer" OFF)

include(Sanitizers)

# On non-Windows platforms, Clang uses static ASan by default which can cause linking issues.
# Use shared ASan runtime instead. This flag is not valid on Windows (clang-cl).
if(NOT WIN32)
    add_compile_options("$<$<CXX_COMPILER_ID:Clang>:-shared-libasan>")
endif()

endif(ENABLE_SANITIZERS)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

# Template the git commit information prior to building the application.
set(GIT_FAIL_IF_NONZERO_EXIT false)
set(PRE_CONFIGURE_FILE "src/core/version/version_git.constants.cpp.in")
set(POST_CONFIGURE_FILE "${CMAKE_CURRENT_BINARY_DIR}/src/core/version/version_git.constants.cpp")
include(GitWatcher)

# Template the versioning information prior to building the application.
set(VERSION_CMAKE_SOURCE_FILE "src/core/version/version_cmake.constants.cpp.in")
set(VERSION_CMAKE_DEST_FILE "${CMAKE_CURRENT_BINARY_DIR}/src/core/version/version_cmake.constants.cpp")
include(CMakeVersionWatcher)

# Mark the source files as generated so that the add_XXXXX command doesn't complain
set_source_files_properties(${POST_CONFIGURE_FILE} PROPERTIES GENERATED 1)
set_source_files_properties(${VERSION_CMAKE_DEST_FILE} PROPERTIES GENERATED 1)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

add_subdirectory(src)

#------------------------------------------------------------------------------
#
#    INTERPROCEDURAL OPTIMISATION (LTO/IPO)
#    ======================================
#
#    Enable IPO for the Release configuration on the application targets when the
#    toolchain supports it. Applied per-config so debug/coverage/sanitizer builds
#    (CMAKE_BUILD_TYPE != Release) are unaffected.
#
#------------------------------------------------------------------------------

if(AQ_IPO_SUPPORTED)
	foreach(_aq_ipo_target IN ITEMS libaqualink-automate libaqualink-jandy libaqualink-pentair aqualink-automate)
		set_target_properties(${_aq_ipo_target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	endforeach()
endif()

#------------------------------------------------------------------------------
#
#    STATIC ANALYSIS CONFIGURATION
#    =============================
#
#------------------------------------------------------------------------------

if(ENABLE_CLANG_TIDY)

find_package(ClangTidy)

# Apply clang-tidy to both the library and executable
set_target_properties(libaqualink-automate PROPERTIES CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY}")
set_target_properties(aqualink-automate PROPERTIES CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY}")

endif(ENABLE_CLANG_TIDY)

if(ENABLE_PVS_STUDIO)

include(PVS-Studio)

pvs_studio_add_target(
	ALL
	TARGET aqualink-automate.analyze
	ANALYZE aqualink-automate libaqualink-automate 
	OUTPUT
	LOG "${CMAKE_CURRENT_BINARY_DIR}/aqualink-automate.pvs-studio.log"
	FORMAT fullhtml 
	MODE 64+GA+OP+CS+MISRA+AUTOSAR+OWASP
	SOURCES "${CMAKE_SOURCE_DIR}/src"
	ARGS --exclude-path "${CMAKE_SOURCE_DIR}/build"
)

endif(ENABLE_PVS_STUDIO)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

if(ENABLE_TESTS)

enable_testing()
add_subdirectory(test)

endif(ENABLE_TESTS)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

if(ENABLE_SANITIZERS)

# Instrument the protocol-decode libraries too: the wire-parsing hot paths
# (Jandy/Pentair framing, checksum, byte-scan) are exactly where OOB reads and
# UB are most likely, so they must carry the sanitizer flags rather than relying
# on the application/core target alone.
target_enable_sanitizers(libaqualink-automate)
target_enable_sanitizers(libaqualink-jandy)
target_enable_sanitizers(libaqualink-pentair)
target_enable_sanitizers(aqualink-automate)

endif(ENABLE_SANITIZERS)

#------------------------------------------------------------------------------
#
#    FUZZING CONFIGURATION
#    =====================
#
#    Opt-in (ENABLE_FUZZING) libFuzzer harnesses over the untrusted RS-485 wire
#    decoders. The protocol-decode libraries carry the fuzzer coverage + ASan
#    instrumentation (the wire-parsing hot paths are exactly where OOB reads live);
#    the harness executables live in fuzz/ and add the libFuzzer engine. Compiler-
#    conditional flags live in cmake/Fuzzing.cmake, keeping src/ CMake platform-
#    and compiler-agnostic (see docs/platform-isolation.md). See docs/fuzzing.md.
#
#------------------------------------------------------------------------------

if(ENABLE_FUZZING)

include(Fuzzing)

target_enable_fuzzing_instrumentation(libaqualink-automate)
target_enable_fuzzing_instrumentation(libaqualink-jandy)
target_enable_fuzzing_instrumentation(libaqualink-pentair)

# The fuzzing preset builds with ENABLE_TESTS OFF (the harnesses are the only
# targets), so enable_testing() is not reached via the ENABLE_TESTS block above.
# Enable it here so fuzz/ can register the harnesses as CTest tests (`ctest`
# then runs a bounded fuzz / corpus replay). enable_testing() is idempotent.
enable_testing()

add_subdirectory(fuzz)

endif(ENABLE_FUZZING)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

include(CPackConfig)
