#include <cctype>
#include <charconv>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>

#include "developer/mock_serial_port_impl.h"

namespace AqualinkAutomate::Developer
{
	MockSerialPortImpl::MockSerialPortImpl() :
		m_Distribution(32, 127),
		m_ProfilingDomain(std::move(Factory::ProfilerFactory::Instance().Get()->CreateDomain("MockSerialPortImpl")))
	{
	}

	MockSerialPortImpl::MockSerialPortImpl(const std::string& device_name)
		: MockSerialPortImpl()
	{
		open(device_name);
	}

	MockSerialPortImpl::MockSerialPortImpl(const std::string& device_name, boost::system::error_code& ec)
		: MockSerialPortImpl()
	{
		open(device_name, ec);
	}

	MockSerialPortImpl::~MockSerialPortImpl()
	{
		boost::system::error_code ec;
		// Qualify to select this class's own close explicitly: a virtual dispatch
		// from a destructor already resolves to MockSerialPortImpl::close, and that
		// base implementation (which tears down this class's own m_File/m_IsOpen) is
		// the intended target regardless of any future subclass.
		MockSerialPortImpl::close(ec);
	}

	void MockSerialPortImpl::open(const std::string& device_name)
	{
		// We're not actually opening a real serial port, so this function
		// doesn't do anything except set the "is_open" flag.

		boost::system::error_code ec;
		open(device_name, ec);
		if (ec)
		{
			throw boost::system::system_error(ec);
		}
	}

	void MockSerialPortImpl::open(const std::string& device_name, boost::system::error_code& ec)
	{
		// We're not actually opening a real serial port, so this function
		// doesn't do anything except set the "is_open" flag.

		ec = {};

		if (is_open())
		{
			ec = boost::asio::error::already_open;
		}
		else if (std::filesystem::exists(device_name) && (m_File.open(device_name), m_File.is_open()))
		{
			m_MockData = false;
			m_IsOpen = true;
			m_DeviceName = device_name;
		}
		else
		{
			m_MockData = true;
			m_IsOpen = true;
			m_DeviceName = device_name;
		}
	}

	bool MockSerialPortImpl::is_open() const
	{
		return m_IsOpen;
	}

	void MockSerialPortImpl::cancel()
	{
		boost::system::error_code ec;
		cancel(ec);
		if (ec)
		{
			throw boost::system::system_error(ec);
		}
	}

	void MockSerialPortImpl::cancel(boost::system::error_code& ec)
	{
		ec = {};

		if (!m_IsOpen)
		{
			ec = boost::asio::error::bad_descriptor;
		}
		else
		{
			m_Cancelled = true;
		}
	}

	void MockSerialPortImpl::close()
	{
		// Again, we're not actually closing a real serial port, so this
		// function just sets the "is_open" flag to false.

		boost::system::error_code ec;
		close(ec);
		if (ec)
		{
			throw boost::system::system_error(ec);
		}
	}

	void MockSerialPortImpl::close(boost::system::error_code& ec)
	{
		// Again, we're not actually closing a real serial port, so this
		// function just sets the "is_open" flag to false.

		ec = {};

		m_DeviceName.clear();
		m_IsOpen = false;
		m_MockData = true;

		try
		{
			if (m_File && m_File.is_open()) m_File.close();
		}
		catch (...) // NOSONAR(cpp:S1181) — boundary: the error_code overload must never throw (its no-throw contract is relied on by the destructor); any close failure is reported via ec.
		{
			ec = boost::asio::error::operation_aborted;
		}
	}

	void MockSerialPortImpl::set_baud_rate(uint32_t rate, boost::system::error_code& ec)
	{
		ec = {};
		m_Options.baud_rate = rate;
	}

	void MockSerialPortImpl::set_character_size(uint8_t bits, boost::system::error_code& ec)
	{
		ec = {};
		m_Options.character_size = bits;
	}

	void MockSerialPortImpl::set_flow_control(Serial::FlowControl fc, boost::system::error_code& ec)
	{
		ec = {};
		m_Options.flow_control = fc;
	}

	void MockSerialPortImpl::set_parity(Serial::Parity p, boost::system::error_code& ec)
	{
		ec = {};
		m_Options.parity = p;
	}

	void MockSerialPortImpl::set_stop_bits(Serial::StopBits sb, boost::system::error_code& ec)
	{
		ec = {};
		m_Options.stop_bits = sb;
	}

	void MockSerialPortImpl::set_read_timeout(std::chrono::milliseconds timeout, boost::system::error_code& ec)
	{
		ec = {};
	}

	std::size_t MockSerialPortImpl::read_some(const boost::asio::mutable_buffer& buffer)
	{
		boost::system::error_code ec;
		auto bytes = read_some(buffer, ec);
		if (ec)
		{
			throw boost::system::system_error(ec, "read_some");
		}
		return bytes;
	}

	std::size_t MockSerialPortImpl::read_some(const boost::asio::mutable_buffer& buffer, boost::system::error_code& ec)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MockSerialPortImpl::read_some", std::source_location::current());

		ec = boost::system::error_code{};
		auto bytes_transferred = std::size_t{ 0 };

		if (!m_IsOpen)
		{
			ec = boost::asio::error::bad_descriptor;
		}
		else if (!m_MockData)
		{
			if (auto result = HandleFileRead(buffer); result)
			{
				bytes_transferred = *result;
			}
			else
			{
				ec = result.error();
			}
		}
		else
		{
			if (auto result = HandleMockRead(buffer); result)
			{
				bytes_transferred = *result;
			}
			else
			{
				ec = result.error();
			}
		}

		return bytes_transferred;
	}

	std::size_t MockSerialPortImpl::write_some(const boost::asio::const_buffer& buffer)
	{
		boost::system::error_code ec;
		auto bytes = write_some(buffer, ec);
		if (ec)
		{
			throw boost::system::system_error(ec, "write_some");
		}
		return bytes;
	}

	std::size_t MockSerialPortImpl::write_some(const boost::asio::const_buffer& buffer, boost::system::error_code& ec)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MockSerialPortImpl::write_some", std::source_location::current());

		ec = boost::system::error_code{};
		auto bytes_transferred = std::size_t{ 0 };

		if (!m_IsOpen)
		{
			ec = boost::asio::error::bad_descriptor;
		}
		else if (!m_MockData)
		{
			if (auto result = HandleFileWrite(buffer); result)
			{
				bytes_transferred = *result;
			}
			else
			{
				ec = result.error();
			}
		}
		else
		{
			if (auto result = HandleMockWrite(buffer); result)
			{
				bytes_transferred = *result;
			}
			else
			{
				ec = result.error();
			}
		}

		return bytes_transferred;
	}

	std::expected<std::size_t, boost::system::error_code> MockSerialPortImpl::HandleMockRead(const boost::asio::mutable_buffer& buffer)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MockSerialPortImpl::HandleMockRead", std::source_location::current());

		const auto length_to_copy = std::min<std::size_t>(buffer.size(), 16);

		auto* buffer_begin = static_cast<uint8_t*>(buffer.data());
		auto* buffer_end = buffer_begin + length_to_copy;

		std::generate(buffer_begin, buffer_end, [&]() -> uint8_t
			{
				return static_cast<uint8_t>(m_Distribution(m_RandomDevice));
			}
		);

		return length_to_copy;
	}

	std::expected<std::size_t, boost::system::error_code> MockSerialPortImpl::HandleMockWrite(const boost::asio::const_buffer& buffer)
	{
		std::size_t bytes_transferred = 0;

		auto convert_stop_bits_to_number = [](Serial::StopBits stop_bits)
			{
				using enum Serial::StopBits;
				switch (stop_bits)
				{
				case One: return 1.0;
				case OnePointFive: return 1.5;
				case Two: return 2.0;
				default: LogWarning(Channel::Serial, "Attempted to convert an invalid stop bit value; assuming one (1)"); return 1.0;
				}
			};

		auto convert_parity_bits_to_number = [](Serial::Parity the_parity)
			{
				using enum Serial::Parity;
				switch (the_parity)
				{
				case None:
					return 0;

				case Odd:
					[[fallthrough]];
				case Even:
					return 1;

				default:
					LogWarning(Channel::Serial, "Attempted to convert an invalid parity value; assuming none (0)");
					return 0;
				}
			};

		// The total bits transmitted per character is: start_bit + data_bits + optional_parity_bit + stop_bits
		const double bits_per_sent_byte = 1.0 + static_cast<double>(m_Options.character_size) + static_cast<double>(convert_parity_bits_to_number(m_Options.parity)) + convert_stop_bits_to_number(m_Options.stop_bits);
		const double number_of_bits_to_send = bits_per_sent_byte * static_cast<double>(buffer.size());
		const auto bits_per_msec = static_cast<double>(m_Options.baud_rate) / 1000.0;
		const auto send_duration = std::chrono::milliseconds(static_cast<long long>(number_of_bits_to_send / bits_per_msec));

		std::this_thread::sleep_for(send_duration);
		bytes_transferred = buffer.size();

		return bytes_transferred;
	}

	std::expected<std::size_t, boost::system::error_code> MockSerialPortImpl::HandleFileRead(const boost::asio::mutable_buffer& buffer)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MockSerialPortImpl::HandleFileRead", std::source_location::current());

		enum FileReadErrors : std::size_t
		{
			ErrorFileReachedEOF,
			ErrorDuringRead,
			NoDataWasRead,
			ReadSuccessfully
		};

		auto ec = boost::system::error_code{};

		// Hand back a single replay byte.  Bytes are decoded a whole line at a
		// time into m_PendingReplayBytes (so a recording line can span several
		// reads and several lines can coalesce into one read); when that buffer
		// drains we pull the next *usable* line from the file.  Comment/header
		// ('#') lines, blank lines and W-direction (app-output) lines yield no
		// replay bytes and are transparently skipped here.
		auto read_single_value_from_file = [this](auto& source_stream, uint8_t& output_buffer) -> FileReadErrors
			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MockSerialPortImpl::HandleFileRead -> read_single_value", std::source_location::current());

				while (m_PendingReplayBytes.empty())
				{
					if (source_stream.eof())
					{
						return ErrorFileReachedEOF;
					}

					std::string line;
					if (!std::getline(source_stream, line))
					{
						// No more lines: EOF (clean) vs a genuine stream error.
						return source_stream.eof() ? ErrorFileReachedEOF : ErrorDuringRead;
					}

					// DecodeReplayLine refills m_PendingReplayBytes for R/legacy
					// data lines and leaves it empty for skipped lines; loop on to
					// the next line in the latter case rather than reporting EOF.
					(void)DecodeReplayLine(line, m_PendingReplayBytes);
				}

				output_buffer = m_PendingReplayBytes.front();
				m_PendingReplayBytes.pop_front();
				return ReadSuccessfully;
			};

		auto read_from_file = [&](auto& source_stream, uint8_t* output_buffer, std::size_t number_of_elems, boost::system::error_code& out_ec) -> std::size_t
			{
				auto read_block_zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MockSerialPortImpl::HandleFileRead -> read_block", std::source_location::current());

				std::size_t elems_read = 0;
				bool keep_reading = true;

				do
				{
					const FileReadErrors result = read_single_value_from_file(source_stream, output_buffer[elems_read]);
					switch (result)
					{
					case FileReadErrors::ErrorFileReachedEOF:
						out_ec = boost::asio::error::eof;
						keep_reading = false;
						break;

					case FileReadErrors::ErrorDuringRead:
					case FileReadErrors::NoDataWasRead:
						out_ec = boost::asio::error::operation_aborted;
						keep_reading = false;
						break;

					case FileReadErrors::ReadSuccessfully:
						out_ec = make_error_code(boost::system::errc::success);
						elems_read++;
						break;
					}

				} while (keep_reading && (number_of_elems > elems_read));

				// read_some semantics: hand back whatever we managed to read this
				// call and only surface EOF/abort once NO bytes are available.
				// Otherwise the caller (HandleFileRead) discards a partially-filled
				// buffer, dropping the tail of any recording shorter than the read
				// chunk (lossless replay regardless of recording size).
				if (out_ec && (elems_read > 0))
				{
					out_ec = make_error_code(boost::system::errc::success);
				}

				return elems_read;
			};

		const auto length_to_copy = boost::asio::buffer_size(buffer);
		auto length_read = read_from_file(m_File, static_cast<uint8_t*>(buffer.data()), length_to_copy, ec);

		if (ec)
		{
			return std::unexpected(ec);
		}

		return length_read;
	}

	namespace
	{
		// Outcome of classifying a recorder "[<ts>] <DIR> ..." header prefix.
		enum class ReplayHeaderResult
		{
			Continue,	// header stripped from sv; carry on to the token list
			Skip,		// line carries no replayable bytes (return true to caller)
			Error		// line could not be interpreted (return false to caller)
		};

		// Strip the leading "[<timestamp_ms>] <DIR> " header from a recorder data
		// line, mutating sv to leave only the bare 0x##|... token list.  Keeps ONLY
		// R-direction bytes; W-direction bytes are the app's own past output and are
		// skipped.  Behaviour is identical to the inlined block it replaces.
		ReplayHeaderResult StripReplayHeader(std::string_view& sv, const std::string& line)
		{
			const auto close_bracket = sv.find(']');
			if (std::string_view::npos == close_bracket)
			{
				LogWarning(Channel::Serial, std::format("Replay line has '[' but no closing ']'; skipping -> {}", line));
				return ReplayHeaderResult::Error;
			}

			sv.remove_prefix(close_bracket + 1);
			const auto dir_pos = sv.find_first_not_of(" \t");
			if (std::string_view::npos == dir_pos)
			{
				// "[ts]" with no direction/payload (e.g. a metadata-only line that
				// happened to start with a timestamp): nothing to replay.
				return ReplayHeaderResult::Skip;
			}
			sv.remove_prefix(dir_pos);

			const char direction = sv.front();
			if ('W' == direction)
			{
				// App output captured during recording: not part of the input stream.
				return ReplayHeaderResult::Skip;
			}
			if ('R' != direction)
			{
				LogWarning(Channel::Serial, std::format("Replay line has unknown direction '{}' (expected R or W); skipping -> {}", direction, line));
				return ReplayHeaderResult::Error;
			}

			// Advance past the direction char and any following whitespace; the
			// remainder is the bare 0x##|... token list handled by the caller.
			sv.remove_prefix(1);
			const auto bytes_pos = sv.find_first_not_of(" \t");
			sv = (std::string_view::npos == bytes_pos) ? std::string_view{} : sv.substr(bytes_pos);
			return ReplayHeaderResult::Continue;
		}

		// Parse a bare pipe-delimited "0x##|0x##|..." token list into out.  Returns
		// false if any token was malformed (logged + skipped).  Behaviour is
		// identical to the inlined loop it replaces.
		bool DecodeTokenList(std::string_view sv, std::deque<uint8_t>& out, const std::string& line)
		{
			bool all_tokens_ok = true;
			std::size_t token_start = 0;
			while (token_start <= sv.size())
			{
				const auto pipe = sv.find('|', token_start);

				// Expected token format is exactly 0x## (4 chars).
				if (const std::string_view token = sv.substr(token_start, (std::string_view::npos == pipe) ? std::string_view::npos : (pipe - token_start)); (4 == token.size()) && ('0' == token[0]) && (('x' == token[1]) || ('X' == token[1])))
				{
					uint8_t converted_value = 0;
					auto [p, conv_ec] = std::from_chars(token.data() + 2, token.data() + 4, converted_value, 16);
					if (std::errc() == conv_ec)
					{
						out.push_back(converted_value);
					}
					else
					{
						all_tokens_ok = false;
						LogWarning(Channel::Serial, std::format("Could not convert replay token '{}'; skipping token in line -> {}", token, line));
					}
				}
				else if (!token.empty())
				{
					all_tokens_ok = false;
					LogWarning(Channel::Serial, std::format("Replay token not in expected 0x## format ('{}'); skipping token in line -> {}", token, line));
				}

				if (std::string_view::npos == pipe)
				{
					break;
				}
				token_start = pipe + 1;
			}

			return all_tokens_ok;
		}
	}

	bool MockSerialPortImpl::DecodeReplayLine(const std::string& line, std::deque<uint8_t>& out)
	{
		// Trim leading whitespace so indented lines and the leading '[' / '0x'
		// can be classified uniformly.
		std::string_view sv{ line };
		const auto first = sv.find_first_not_of(" \t\r\n");
		if (std::string_view::npos == first)
		{
			// Blank / whitespace-only line: nothing to replay, not an error.
			return true;
		}
		sv.remove_prefix(first);

		// '#'-prefixed comment / header / metadata lines carry no wire bytes.
		if ('#' == sv.front())
		{
			return true;
		}

		// Recorder data line:  [<timestamp_ms>] <DIR> 0x##|0x##|...
		// Strip the "[...]" timestamp and the single direction character, keeping
		// ONLY R-direction bytes (bytes received from the device).  W-direction
		// bytes are the application's own past output and must NOT be replayed as
		// input, so a W line decodes to zero bytes (skipped).
		if ('[' == sv.front())
		{
			using enum ReplayHeaderResult;
			switch (StripReplayHeader(sv, line))
			{
			case Skip:
				return true;
			case Error:
				return false;
			case Continue:
				break;
			}
		}

		// At this point sv is a (possibly empty) bare pipe-delimited token list:
		//   0x##|0x##|...   (legacy format, or the payload of an R data line)
		// Trim a trailing CR (already covered by remove_prefix above for leading
		// whitespace, but getline on a CRLF file leaves the '\r' at the end).
		while (!sv.empty() && (('\r' == sv.back()) || ('\n' == sv.back()) || (' ' == sv.back()) || ('\t' == sv.back())))
		{
			sv.remove_suffix(1);
		}

		if (sv.empty())
		{
			return true;
		}

		return DecodeTokenList(sv, out, line);
	}

	std::expected<std::size_t, boost::system::error_code> MockSerialPortImpl::HandleFileWrite(const boost::asio::const_buffer& /*buffer*/)
	{
		return 0;
	}

}
// namespace AqualinkAutomate::Developer
