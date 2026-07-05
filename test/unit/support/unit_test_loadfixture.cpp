#include "support/unit_test_loadfixture.h"

#include <array>
#include <filesystem>
#include <format>
#include <stdexcept>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include "developer/mock_serial_port_impl.h"

#include "support/unit_test_mockreplayharness.h"

namespace AqualinkAutomate::Test
{

	std::vector<uint8_t> LoadFixture(const std::string& fixture_path)
	{
		if (!std::filesystem::exists(fixture_path))
		{
			throw std::runtime_error(std::format("LoadFixture: capture file not found: {} (cwd: {})",
				fixture_path, std::filesystem::current_path().string()));
		}

		// Drive the bytes out through the REAL production file reader so the loaded
		// stream is identical to what a live `--replay-filename` run would deliver.
		// MockSerialPortImpl::open() switches to file mode when the path exists.
		Developer::MockSerialPortImpl port;
		boost::system::error_code ec;
		port.open(fixture_path, ec);
		if (ec)
		{
			throw std::runtime_error(std::format("LoadFixture: failed to open capture file {}: {}", fixture_path, ec.message()));
		}

		std::vector<uint8_t> bytes;
		std::array<uint8_t, 256> chunk{};

		for (;;)
		{
			boost::system::error_code read_ec;
			const auto n = port.read_some(boost::asio::buffer(chunk), read_ec);

			if (n > 0)
			{
				bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
			}

			if (read_ec)
			{
				// eof => clean end of capture; anything else => stop too (no more
				// usable data).  Either way we have everything readable.
				break;
			}

			if (0 == n)
			{
				break;
			}
		}

		return bytes;
	}

	std::vector<uint8_t> ReplayFixture(MockReplayHarness& harness, const std::string& fixture_path)
	{
		auto bytes = LoadFixture(fixture_path);
		harness.Replay(bytes);
		return bytes;
	}

	std::vector<std::vector<uint8_t>> LoadFixtureFrames(const std::string& fixture_path)
	{
		constexpr uint8_t DLE = 0x10, STX = 0x02, ETX = 0x03;

		const auto bytes = LoadFixture(fixture_path);
		std::vector<std::vector<uint8_t>> frames;

		std::size_t i = 0;
		while (i + 1 < bytes.size())
		{
			if (!(bytes[i] == DLE && bytes[i + 1] == STX))
			{
				++i;
				continue;
			}

			// Scan for the closing DLE/ETX, skipping DLE/00 stuffed data bytes so a
			// literal 0x10 in the payload is not mistaken for the frame terminator.
			std::size_t j = i + 2;
			while (j + 1 < bytes.size())
			{
				if (bytes[j] == DLE && bytes[j + 1] == ETX) { break; }
				if (bytes[j] == DLE && bytes[j + 1] == 0x00) { j += 2; continue; }
				++j;
			}

			if (j + 1 < bytes.size() && bytes[j] == DLE && bytes[j + 1] == ETX)
			{
				frames.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(i),
					bytes.begin() + static_cast<std::ptrdiff_t>(j + 2));
				i = j + 2;
			}
			else
			{
				break; // no terminator -> trailing partial data, stop
			}
		}

		return frames;
	}

	std::vector<std::vector<uint8_t>> ReplayFixtureFramed(MockReplayHarness& harness, const std::string& fixture_path)
	{
		auto frames = LoadFixtureFrames(fixture_path);
		harness.Replay(frames);
		return frames;
	}

}
// namespace AqualinkAutomate::Test
