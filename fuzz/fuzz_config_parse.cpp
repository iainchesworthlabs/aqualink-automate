//
// libFuzzer harness: config-file reading + option-value validation.
//
// The app reads an optional INI-style config file (flat key=value, keys = option
// long-names) and merges it into a variables_map, running each option's value
// through boost.program_options plus the project's custom validators
// (Severity / ProfilerTypes / SyslogFacility / MQTT ProtocolVersion). The
// production config path (Options::ParseConfigFile) narrows its exception handling
// to boost::program_options::error and std::filesystem_error only — so a validator
// (or the INI parser) that threw ANY OTHER exception on a malformed config value
// would escape and crash the app at startup.
//
// This harness feeds arbitrary bytes as config-file text through the SAME
// boost::program_options config parser + custom validators, over a representative
// options_description (not the full registry — a grammar exercising each custom
// validator plus plain string/int/bool_switch options). Expected rejections
// (boost::program_options::error) are swallowed; any other exception escapes and
// libFuzzer flags it as the real bug. Build: fuzz/CMakeLists.txt.
//

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/errors.hpp>

// Each validator header declares the custom validate() overload (found by ADL on
// the value<T>() type at store() time) and pulls in its enum type.
#include "options/validators/severity_level_validator.h"     // Logging::Severity
#include "options/validators/profiler_type_validator.h"      // Types::ProfilerTypes
#include "options/validators/syslog_facility_validator.h"    // Logging::Sinks::SyslogFacility
#include "options/options_mqtt_options.h"                    // Options::Mqtt::ProtocolVersion
#include "logging/logging_severity_filter.h"

namespace po = boost::program_options;
using namespace AqualinkAutomate;

namespace
{
	// A representative grammar: one option per custom-validated type (so arbitrary
	// config text drives those validators) plus plain option kinds. Built once.
	const po::options_description& FuzzDescription()
	{
		static const po::options_description desc = []
		{
			po::options_description d("fuzz");
			d.add_options()
				("loglevel-main", po::value<Logging::Severity>(), "")
				("profiler", po::value<Types::ProfilerTypes>(), "")
				("log-syslog-facility", po::value<Logging::Sinks::SyslogFacility>(), "")
				("mqtt-protocol-version", po::value<Options::Mqtt::ProtocolVersion>(), "")
				("name", po::value<std::string>(), "")
				("count", po::value<int>(), "")
				("verbose", po::bool_switch(), "");
			return d;
		}();
		return desc;
	}
}
// unnamed namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	std::istringstream stream(std::string(reinterpret_cast<const char*>(data), size));
	po::variables_map vm;

	try
	{
		// allow_unregistered=true mirrors DoParseConfigFile (unknown keys warn, not throw).
		auto parsed = po::parse_config_file(stream, FuzzDescription(), /*allow_unregistered=*/true);
		po::store(parsed, vm);   // invokes the custom validators for present options
		po::notify(vm);
	}
	catch (const po::error&)
	{
		// Expected: malformed config syntax or an invalid option value. Anything
		// NOT derived from po::error is deliberately left to escape (a bug).
	}

	return 0;
}
