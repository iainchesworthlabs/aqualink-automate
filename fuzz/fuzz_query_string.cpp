//
// libFuzzer harness: HTTP query-string / request-target parsing.
//
// HTTP::ParseQueryString extracts a named query parameter from an untrusted
// request target (URL) via boost::urls::parse_origin_form. This harness sets the
// fuzzed bytes as the request target of a Boost.Beast request and extracts a
// parameter, exercising the URL parse + param lookup. A malformed target must
// yield std::unexpected, never crash. Build: fuzz/CMakeLists.txt.
//

#include <cstddef>
#include <cstdint>
#include <string>

#include "http/server/parse_query_string.h"
#include "http/server/server_types.h"   // HTTP::Request (boost::beast request)
#include "logging/logging_severity_filter.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// The target setter copies the bytes into the message; ParseQueryString does
	// the parsing (boost::urls::parse_origin_form on the stored target).
	const std::string target(reinterpret_cast<const char*>(data), size);

	HTTP::Request request;
	request.method(boost::beast::http::verb::get);
	request.target(target);

	(void)HTTP::ParseQueryString(request, "id");

	return 0;
}
