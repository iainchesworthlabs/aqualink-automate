//
// libFuzzer harness: schedule request-body JSON validators.
//
// The web API turns an untrusted JSON request body into a domain object through
// two first-party validators (POST/PUT /api/schedules and
// /api/controller/schedules): Scheduling::FromJson and
// Scheduling::ControllerScheduleFromJson. Both document the contract "return
// std::nullopt and set `error` on any invalid field" — i.e. they must NEVER throw
// on malformed input. The HTTP handlers rely on that: the body is parsed with
// nlohmann allow_exceptions=false and the validator is then called with no
// try/catch, so a throw escapes as an uncaught exception on attacker input.
//
// This harness feeds arbitrary bytes through the same parse + both validators.
// A throw / crash here is a real bug — fix the validator to honour its contract,
// never weaken it. (It already found FromJson throwing nlohmann type_error.302 on a
// present-but-wrong-typed "name"/"enabled"/action field — now fixed + regression-
// tested in test/unit/scheduling/test_schedule.cpp.)
//
// Build: see fuzz/CMakeLists.txt (ENABLE_FUZZING). Third-party nlohmann parse and
// Boost.URL etc. are already fuzzed upstream; the value here is the validators.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "scheduling/schedule.h"
#include "scheduling/controller_schedule.h"
#include "logging/logging_severity_filter.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// Parse exactly as the HTTP handlers do: allow_exceptions=false, so a malformed
	// document yields a discarded value rather than throwing.
	const auto json = nlohmann::json::parse(
		std::string_view(reinterpret_cast<const char*>(data), size),
		nullptr, /*allow_exceptions=*/false);

	if (json.is_discarded())
	{
		return 0;
	}

	std::string error;

	// Neither validator may throw on any input — a throw is a contract violation and
	// a real crash on an untrusted request body. Do NOT wrap in try/catch: an escaping
	// exception is precisely what this harness exists to catch.
	(void)Scheduling::FromJson(json, error);
	(void)Scheduling::ControllerScheduleFromJson(json, error);

	return 0;
}
