//
// libFuzzer harness: inbound MQTT command-payload parsers.
//
// A broker delivers untrusted command payloads that MqttHub decodes via the pure
// helpers in mqtt_payload_parsing.h: ParsePayloadNumber<T> (range-checked numeric
// extraction from a JSON number / string / {"raw": "..."} envelope via
// std::from_chars), ParsePayloadString, and SanitiseForLog. This harness parses
// arbitrary bytes as JSON exactly as MqttHub::ProcessCommand does
// (allow_exceptions=false) and drives every helper across a spread of target types.
//
// These are non-throwing by design (from_chars, range clamping); a throw / crash
// here is a real bug on the MQTT hot path. Build: fuzz/CMakeLists.txt.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "mqtt/mqtt_payload_parsing.h"
#include "logging/logging_severity_filter.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const auto payload = nlohmann::json::parse(
		std::string_view(reinterpret_cast<const char*>(data), size),
		nullptr, /*allow_exceptions=*/false);

	if (payload.is_discarded())
	{
		return 0;
	}

	// Numeric extraction across signed/unsigned/floating target domains (range
	// checks + rounding paths), plus string extraction and the log sanitiser.
	(void)Mqtt::PayloadParsing::ParsePayloadNumber<uint8_t>(payload);
	(void)Mqtt::PayloadParsing::ParsePayloadNumber<int32_t>(payload);
	(void)Mqtt::PayloadParsing::ParsePayloadNumber<uint32_t>(payload);
	(void)Mqtt::PayloadParsing::ParsePayloadNumber<double>(payload);

	const std::string as_string = Mqtt::PayloadParsing::ParsePayloadString(payload);
	(void)Mqtt::PayloadParsing::SanitiseForLog(as_string);

	return 0;
}
