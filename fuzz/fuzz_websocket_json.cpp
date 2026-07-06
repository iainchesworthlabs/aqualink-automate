//
// libFuzzer harness: inbound WebSocket message envelope parser.
//
// Every connected browser feeds untrusted text frames to
// HTTP::WebSocket_Event::ConvertFromStringView, which JSON-parses the frame,
// checks the {type, payload} envelope, and case-insensitively enum-casts the
// event type. This is a pure string_view -> optional<Event> function — an easy,
// high-value target: it already carries a fixed platform-specific decode bug (a
// gcc/libstdc++ brace-init that wrapped the parsed object in a 1-element array and
// silently rejected every event on Linux while passing on MSVC), so fuzzing it
// under the real Clang/libFuzzer build guards against that class of regression.
//
// A crash / uncaught exception here is a real bug — the parser must reject a
// malformed frame with std::nullopt, never throw. Build: fuzz/CMakeLists.txt.
//

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "http/websocket_event.h"
#include "logging/logging_severity_filter.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	(void)HTTP::WebSocket_Event::ConvertFromStringView(
		std::string_view(reinterpret_cast<const char*>(data), size));

	return 0;
}
