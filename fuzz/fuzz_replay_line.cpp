//
// libFuzzer harness: record/replay .cap capture-file parser.
//
// The developer record/replay feature reads an on-disk .cap capture (INI-ish text:
// "# comment", "[<ts>] <DIR> 0x##|0x##|...", and legacy bare "0x##|..." lines) and
// replays the R-direction bytes. The line parser (MockSerialPortImpl::DecodeReplayLine)
// is private, so — exactly like test/unit/serial/test_mock_serial_replay_format.cpp —
// this harness drives it through the PUBLIC file path: it writes the fuzzed bytes to
// a .cap file, opens a MockSerialPortImpl against it (file/replay mode), and drains
// every byte. That also exercises the multi-read chunk buffering. A .cap file is a
// local/developer input (low threat); this guards robustness against a malformed
// capture. Build: fuzz/CMakeLists.txt.
//

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include "developer/mock_serial_port_impl.h"
#include "logging/logging_severity_filter.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// Reused/truncated temp capture file (one per process, rewritten each input).
	static const std::filesystem::path cap_path =
		std::filesystem::temp_directory_path() / "aa_fuzz_replay.cap";

	{
		std::ofstream out(cap_path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	}

	Developer::MockSerialPortImpl port;
	boost::system::error_code ec;
	port.open(cap_path.string(), ec);
	if (ec || !port.is_open())
	{
		return 0;
	}

	std::array<uint8_t, 8> chunk{}; // small chunk to exercise multi-read buffering
	for (;;)
	{
		boost::system::error_code read_ec;
		const auto n = port.read_some(boost::asio::buffer(chunk), read_ec);
		if (read_ec || (0 == n))
		{
			break;
		}
	}

	return 0;
}
