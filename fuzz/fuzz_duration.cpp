//
// libFuzzer harness: HH:MM:SS timeout-duration string parser.
//
// Utility::TimeoutDurationStringConverter parses an operator-supplied "HH:MM:SS"
// option value (fixed 8 chars, ':' at indices 2 and 5, numeric fields via
// std::from_chars). It is entirely noexcept, so the only failure mode a fuzzer can
// surface is an out-of-bounds index / crash on a malformed value. Low threat
// (config value), included for completeness. Build: fuzz/CMakeLists.txt.
//

#include <cstddef>
#include <cstdint>
#include <string>

#include "utility/timeout_duration_string_converter.h"
#include "logging/logging_severity_filter.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const Utility::TimeoutDurationStringConverter converter(
		std::string(reinterpret_cast<const char*>(data), size));
	(void)converter();

	return 0;
}
