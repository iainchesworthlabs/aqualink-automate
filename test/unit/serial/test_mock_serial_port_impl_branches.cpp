#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>
#include <boost/test/unit_test.hpp>

#include "developer/mock_serial_port_impl.h"
#include "serial/serial_port_enums.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Developer;

//=============================================================================
// MockSerialPortImpl branch coverage: the remaining port-settings setters, the
// mock-write timing model's non-default parity/stop-bit arms, file-mode writes,
// and the replay-line parser's malformed-header / whitespace edge cases.
//=============================================================================

namespace
{
	struct TempCaptureFile
	{
		std::filesystem::path path;

		explicit TempCaptureFile(const std::string& contents)
		{
			static int counter = 0;
			path = std::filesystem::temp_directory_path() /
				std::filesystem::path(std::string("aa_mock_branches_") + std::to_string(++counter) + ".cap");

			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << contents;
		}

		~TempCaptureFile()
		{
			std::error_code ec;
			std::filesystem::remove(path, ec);
		}

		TempCaptureFile(const TempCaptureFile&) = delete;
		TempCaptureFile& operator=(const TempCaptureFile&) = delete;
	};

	std::vector<uint8_t> DrainReplay(MockSerialPortImpl& port)
	{
		std::vector<uint8_t> out;
		std::array<uint8_t, 4> chunk{};

		for (;;)
		{
			boost::system::error_code read_ec;
			const auto n = port.read_some(boost::asio::buffer(chunk), read_ec);
			if (n > 0)
			{
				out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
			}
			if (read_ec || (0 == n))
			{
				break;
			}
		}

		return out;
	}
}
// anonymous namespace

BOOST_AUTO_TEST_SUITE(MockSerialPortImplBranches_TestSuite)

// -----------------------------------------------------------------------------
// Remaining setters clear a pre-existing error code
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(FlowControlParityStopBits_ClearPreExistingError)
{
	MockSerialPortImpl port("mock");

	boost::system::error_code ec = boost::asio::error::fault;
	port.set_flow_control(Serial::FlowControl::Hardware, ec);
	BOOST_CHECK(!ec);

	ec = boost::asio::error::fault;
	port.set_parity(Serial::Parity::Odd, ec);
	BOOST_CHECK(!ec);

	ec = boost::asio::error::fault;
	port.set_stop_bits(Serial::StopBits::Two, ec);
	BOOST_CHECK(!ec);
}

// -----------------------------------------------------------------------------
// Mock write timing model: each parity / stop-bit arm still transfers the
// whole buffer (a high baud rate keeps the simulated line delay negligible).
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(MockWrite_ParityAndStopBitVariants_TransferWholeBuffer)
{
	struct Variant { Serial::Parity parity; Serial::StopBits stop_bits; };
	const Variant variants[] = {
		{ Serial::Parity::Odd,  Serial::StopBits::OnePointFive },
		{ Serial::Parity::Even, Serial::StopBits::Two },
		{ Serial::Parity::None, Serial::StopBits::One },
	};

	for (const auto& v : variants)
	{
		MockSerialPortImpl port("mock");
		boost::system::error_code ec;
		port.set_baud_rate(4'000'000, ec);
		port.set_parity(v.parity, ec);
		port.set_stop_bits(v.stop_bits, ec);
		port.set_character_size(8, ec);

		const std::array<uint8_t, 5> data{ 1, 2, 3, 4, 5 };
		BOOST_CHECK_EQUAL(port.write_some(boost::asio::buffer(data), ec), data.size());
		BOOST_CHECK(!ec);
		BOOST_CHECK_EQUAL(port.write_some(boost::asio::buffer(data)), data.size());
	}
}

// -----------------------------------------------------------------------------
// File (replay) mode: writes are swallowed, reads decode the capture
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(FileMode_WriteSome_IsSwallowedWithoutError)
{
	TempCaptureFile cap("0x10|0x02\n");
	MockSerialPortImpl port;

	boost::system::error_code ec;
	port.open(cap.path.string(), ec);
	BOOST_REQUIRE(!ec);
	BOOST_REQUIRE(port.is_open());

	const std::array<uint8_t, 3> data{ 0xAA, 0xBB, 0xCC };
	ec = boost::asio::error::fault;
	BOOST_CHECK_EQUAL(port.write_some(boost::asio::buffer(data), ec), 0u);
	BOOST_CHECK(!ec);
	BOOST_CHECK_EQUAL(port.write_some(boost::asio::buffer(data)), 0u);

	// The replay stream is unaffected by the swallowed writes.
	const auto bytes = DrainReplay(port);
	const std::vector<uint8_t> expected{ 0x10, 0x02 };
	BOOST_CHECK_EQUAL_COLLECTIONS(bytes.begin(), bytes.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(ReplayParser_MalformedHeaders_AreSkipped_GoodLinesSurvive)
{
	// Each malformed / empty header variant must be skipped without aborting
	// the replay, and the well-formed lines around them replay in order.
	TempCaptureFile cap(
		"0x01\n"                         // legacy, good
		"[123\n"                         // '[' with no closing ']' -> skipped
		"[100]\n"                        // timestamp only, no direction -> skipped
		"[100] X 0x99\n"                 // unknown direction -> skipped
		"[100] R\n"                      // R with no payload -> skipped (no bytes)
		"[100] R   \n"                   // R with only whitespace -> skipped
		"   0x02|0x03   \r\n"            // leading/trailing whitespace + CR -> trimmed
		"[101] R 0x04\t\n"               // trailing tab -> trimmed
		"[102] W 0xEE\n"                 // app output -> skipped
		"0x05\n");                       // legacy, good

	MockSerialPortImpl port;
	boost::system::error_code ec;
	port.open(cap.path.string(), ec);
	BOOST_REQUIRE(!ec);

	const auto bytes = DrainReplay(port);
	const std::vector<uint8_t> expected{ 0x01, 0x02, 0x03, 0x04, 0x05 };
	BOOST_CHECK_EQUAL_COLLECTIONS(bytes.begin(), bytes.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(ReplayParser_UppercaseHexPrefix_AndEmptyTokens_Handled)
{
	// "0X" prefix is accepted; an empty token between pipes is ignored while
	// the remaining tokens still decode.
	TempCaptureFile cap("0X10|0x02||0x03\n");

	MockSerialPortImpl port;
	boost::system::error_code ec;
	port.open(cap.path.string(), ec);
	BOOST_REQUIRE(!ec);

	const auto bytes = DrainReplay(port);
	const std::vector<uint8_t> expected{ 0x10, 0x02, 0x03 };
	BOOST_CHECK_EQUAL_COLLECTIONS(bytes.begin(), bytes.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(FileMode_ExhaustedCapture_ReportsEofThenCloseReopensAsMock)
{
	TempCaptureFile cap("0x7F\n");

	MockSerialPortImpl port(cap.path.string());
	BOOST_REQUIRE(port.is_open());

	std::array<uint8_t, 8> chunk{};
	boost::system::error_code ec;
	BOOST_CHECK_EQUAL(port.read_some(boost::asio::buffer(chunk), ec), 1u);
	BOOST_CHECK(!ec);

	// Capture drained: EOF is surfaced once no bytes remain.
	BOOST_CHECK_EQUAL(port.read_some(boost::asio::buffer(chunk), ec), 0u);
	BOOST_CHECK(ec == boost::asio::error::eof);
	BOOST_CHECK_THROW(port.read_some(boost::asio::buffer(chunk)), boost::system::system_error);

	// close() releases the file; re-opening with a non-file name falls back to
	// the random mock data source.
	BOOST_CHECK_NO_THROW(port.close());
	BOOST_CHECK(!port.is_open());
	port.open("not_a_file", ec);
	BOOST_CHECK(!ec);
	BOOST_CHECK_EQUAL(port.read_some(boost::asio::buffer(chunk), ec), chunk.size());
	BOOST_CHECK(!ec);
}

BOOST_AUTO_TEST_SUITE_END()
