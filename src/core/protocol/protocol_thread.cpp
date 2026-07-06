#include <algorithm>
#include <array>
#include <chrono>
#include <format>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/circular_buffer.hpp>

#include "errors/protocol_errors.h"
#include "logging/logging.h"
#include "profiling/profiling.h"
#include "protocol/message_generator_registry.h"
#include "protocol/protocol_handler_constants.h"
#include "protocol/protocol_handler_read.h"
#include "protocol/protocol_thread.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Protocol
{

	namespace
	{
		// A single message taking longer than this to parse + dispatch is flagged
		// as slow.  Lifted from a bare literal so the threshold has one named home.
		constexpr long long SLOW_MESSAGE_THRESHOLD_US = 2000;
	}

	ProtocolTask::ProtocolTask(std::shared_ptr<Serial::SerialPort> serial_port,
							   std::shared_ptr<Kernel::StatisticsHub> statistics_hub,
							   bool single_read_per_poll)
		: m_SerialPort(std::move(serial_port))
		, m_StatisticsHub(std::move(statistics_hub))
		, m_SerialBuffer(Constants::SERIAL_CIRCULAR_BUFFER_SIZE)
		, m_SingleReadPerPoll(single_read_per_poll)
	{
	}

	ProtocolTask::~ProtocolTask()
	{
		m_WriteSignalConnections.clear();
	}

	void ProtocolTask::DrainWrites()
	{
		while (!m_WriteQueue.empty())
		{
			auto& buffer = m_WriteQueue.front();
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ProtocolThread::write_some", std::source_location::current());

			std::size_t bytes_written_this_call = 0;

			while (m_WriteOffset < buffer.size())
			{
				boost::system::error_code ec;
				auto bytes_written = m_SerialPort->write_some(
					boost::asio::buffer(buffer.data() + m_WriteOffset, buffer.size() - m_WriteOffset), ec);

				if (ec)
				{
					if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
					{
						break;  // Try again next iteration
					}
					LogWarning(Channel::Serial, [&ec] { return std::format("Serial write error: {}", ec.message()); });
					if (m_StatisticsHub) { ++m_StatisticsHub->Serial.TransmissionFailures; }
					break;
				}

				bytes_written_this_call += bytes_written;
				m_WriteOffset += bytes_written;
			}

			zone->Value(bytes_written_this_call);
			Factory::ProfilerFactory::Instance().Get()->PlotValue("Serial Bytes Written", static_cast<int64_t>(bytes_written_this_call));

			if (m_WriteOffset >= buffer.size())
			{
				LogTrace(Channel::Serial, [&buffer] { return std::format("Successfully wrote all {} bytes to serial port", buffer.size()); });
				m_WriteQueue.pop_front();
				m_WriteOffset = 0;
			}
			else if (bytes_written_this_call > 0)
			{
				LogDebug(Channel::Serial, [this, &buffer] { return std::format("Write incomplete: wrote {} of {} bytes", m_WriteOffset, buffer.size()); });
				break;  // Partial write; retry remainder next iteration
			}
			else
			{
				break;  // No progress; retry next iteration
			}
		}
	}

	void ProtocolTask::DrainReads()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ProtocolThread::read_some", std::source_location::current());

		std::size_t total_bytes_read = 0;

		// Loop until no more data available — drain the OS serial buffer completely.
		for (;;)
		{
			boost::system::error_code ec;
			auto bytes_read = m_SerialPort->read_some(
				boost::asio::buffer(m_ReadBuffer.data(), m_ReadBuffer.size()), ec);

			if (ec)
			{
				if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
				{
					// No data available — normal for non-blocking
				}
				else if (ec == boost::asio::error::operation_aborted)
				{
					LogTrace(Channel::Serial, "Serial read cancelled (shutdown)");
				}
				else if (ec == boost::asio::error::eof)
				{
					LogWarning(Channel::Serial, "Serial port connection closed by peer");
				}
				else
				{
					LogTrace(Channel::Serial, [&ec] { return std::format("Serial read returned: {}", ec.message()); });
				}
				break;
			}

			if (0 == bytes_read)
			{
				break;
			}

			// Bound the copy to the space that actually fits.  The circular buffer
			// would otherwise EVICT bytes from its FRONT — i.e. the in-flight packet
			// currently being assembled — to make room for the newest bytes,
			// silently corrupting a partially-buffered frame.  Instead, drop the
			// EXCESS NEW bytes (the tail of this read) and count only what is truly
			// discarded.  These dropped bytes are recoverable: a well-formed frame
			// that does not fit now is re-sent by the bus / re-read next poll once
			// the parser has drained the buffer.
			const auto space_available = m_SerialBuffer.capacity() - m_SerialBuffer.size();
			const auto bytes_to_insert = std::min(bytes_read, space_available);

			if (bytes_read > space_available)
			{
				const auto bytes_discarded = bytes_read - space_available;
				if (m_StatisticsHub) { m_StatisticsHub->MessageErrors.BufferOverflows += bytes_discarded; }
				LogWarning(Channel::Protocol, [bytes_discarded] { return std::format("Serial circular buffer full - {} new bytes dropped (in-flight packet preserved)", bytes_discarded); });
			}

			total_bytes_read += bytes_to_insert;

			m_SerialBuffer.insert(m_SerialBuffer.end(),
				m_ReadBuffer.begin(), m_ReadBuffer.begin() + static_cast<std::ptrdiff_t>(bytes_to_insert));

			// Capture-replay pacing: deliver only one chunk per Poll() so the
			// application's frame loop (which steps at a fixed processing period)
			// spaces frame delivery at the bus rate.  Without this bound a replayed
			// file is consumed in a single frame — defeating pacing AND overflowing
			// the circular buffer, since nothing is parsed/consumed between
			// back-to-back reads.  A real serial port self-paces (read_some reports
			// would_block once the OS buffer drains), so production leaves this off.
			if (m_SingleReadPerPoll)
			{
				break;
			}
		}

		if (total_bytes_read > 0)
		{
			zone->Value(total_bytes_read);

			auto profiler = Factory::ProfilerFactory::Instance().Get();
			profiler->PlotValue("Serial Bytes Read", static_cast<int64_t>(total_bytes_read));
			profiler->PlotValue("Serial Buffer Fill", static_cast<int64_t>(m_SerialBuffer.size()));

			LogDebug(Channel::Serial, [total_bytes_read] { return std::format("Read {} bytes from serial port", total_bytes_read); });
		}
	}

	bool ProtocolTask::Poll()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ProtocolTask::Poll", std::source_location::current());

		// Tight loop: read → process → write, so responses are sent in
		// the same frame as the message that triggered them.  Pacing (if any) is
		// owned by the caller's frame loop — never inside read_some, which would
		// break the async read / frame-sync and decode zero frames.

		// 1. Drain any writes queued from the previous frame first.
		DrainWrites();

		// 2. Read → Process → Write loop.
		DrainReads();

		std::size_t messages_parsed = 0;
		if (!m_SerialBuffer.empty())
		{
			// Linearize so that packet subranges are contiguous in memory,
			// enabling zero-copy deserialization via span.
			if (!m_SerialBuffer.is_linearized())
			{
				m_SerialBuffer.linearize();
			}

			messages_parsed = ProcessMessages(m_SerialBuffer);

			// Immediately drain any responses that were queued by message handlers.
			DrainWrites();
		}

		// 3. Sample per-frame metrics for profiler and statistics.
		auto profiler = Factory::ProfilerFactory::Instance().Get();
		profiler->PlotValue("Write Queue Depth", static_cast<int64_t>(m_WriteQueue.size()));
		profiler->PlotValue("Messages Parsed", static_cast<int64_t>(messages_parsed));

		if (m_StatisticsHub)
		{
			m_StatisticsHub->Serial.SerialWriteQueueDepth = m_WriteQueue.size();

			profiler->PlotValue("TX Failures", static_cast<int64_t>(m_StatisticsHub->Serial.TransmissionFailures));
			profiler->PlotValue("Checksum Failures", static_cast<int64_t>(m_StatisticsHub->MessageErrors.ChecksumFailures));
			profiler->PlotValue("Deserialization Failures", static_cast<int64_t>(m_StatisticsHub->MessageErrors.DeserializationFailures));
			profiler->PlotValue("Invalid Packets", static_cast<int64_t>(m_StatisticsHub->MessageErrors.InvalidPacketFormat));
			profiler->PlotValue("Generator Failures", static_cast<int64_t>(m_StatisticsHub->MessageErrors.GeneratorFailures));
			profiler->PlotValue("Overlapping Packets", static_cast<int64_t>(m_StatisticsHub->MessageErrors.OverlappingPackets));
			profiler->PlotValue("Buffer Overflows", static_cast<int64_t>(m_StatisticsHub->MessageErrors.BufferOverflows));
		}

		return (messages_parsed > 0) || !m_WriteQueue.empty();
	}

	void ProtocolTask::EnqueueWrite(std::vector<uint8_t> buffer)
	{
		m_WriteQueue.push_back(std::move(buffer));
	}

	void ProtocolTask::HandleParsedMessage(const ProtocolMessagePtr& message,
										   std::chrono::steady_clock::time_point msg_processing_start,
										   const Types::ProfilerTypePtr& profiler)
	{
		// Successfully parsed a message — fire the signal.
		//
		// Exception barrier: the handler fans the message out to every registered
		// device/status-processor slot via boost::signals2, and the default combiner
		// does NOT swallow exceptions.  A single throwing slot (out_of_range,
		// bad_variant_access, length_error, bad_alloc from an attacker-influenced
		// reserve, …) would otherwise propagate out of the poll loop and terminate
		// the whole daemon — a remote DoS over the RS-485 / remote-serial transport.
		// Mirror the protection the HTTP router already has (routing.cpp): log,
		// count, and keep polling.  A bus frame must never be able to kill the process.
		try
		{
			ProtocolHandler_ReadOp_MessageHandler(message, m_StatisticsHub);
		}
		catch (const std::exception& ex) // NOSONAR(cpp:S1181) — boundary: poll-loop barrier; any throwing signal slot must not kill the daemon (remote-DoS over RS-485).
		{
			if (m_StatisticsHub) { ++m_StatisticsHub->MessageErrors.HandlerExceptions; }
			LogWarning(Channel::Protocol, [&ex] { return std::format(
				"Exception while dispatching a decoded message to its handlers; dropping the message and continuing: {}", ex.what()); });
		}

		const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - msg_processing_start).count();

		if (m_StatisticsHub)
		{
			m_StatisticsHub->LatencyMetrics.MessageProcessingLatency.RecordSince(msg_processing_start);
		}

		// Surface per-message processing latency on the profiler timeline so
		// slow handlers are visible alongside the other per-frame plots.
		profiler->PlotValue("Message Processing (us)", static_cast<int64_t>(elapsed_us));

		if (elapsed_us > SLOW_MESSAGE_THRESHOLD_US)
		{
			LogWarning(Channel::Protocol, [elapsed_us] { return std::format(
				"Slow message processing: took {:.2f} ms", static_cast<double>(elapsed_us) / 1000.0); });
		}
	}

	bool ProtocolTask::HandleGenerationError(const ProtocolErrorCode& error,
											 const ReadOps_SerialBufferType& circular_buffer,
											 std::size_t size_before)
	{
		ProtocolHandler_ReadOp_ErrorHandler(error, m_StatisticsHub);

		// "Keep processing" codes: the generator either cleaned up bad data
		// (and there may be another packet behind it) or signalled that an
		// identified-but-incomplete frame is awaiting more bytes.  Either way
		// we may continue ONLY IF the buffer actually shrank.
		const bool keep_processing =
			(error == make_error_code(ErrorCodes::Protocol_ErrorCodes::DataAvailableToProcess))
			|| (error == make_error_code(ErrorCodes::Protocol_ErrorCodes::ChecksumFailure))
			|| (error == make_error_code(ErrorCodes::Protocol_ErrorCodes::OverlappingPackets));

		if (keep_processing && (circular_buffer.size() < size_before))
		{
			// Forward progress was made — another packet may follow.
			return true;
		}

		if (keep_processing)
		{
			// Bounded recovery: the buffer did NOT shrink.  Two cases:
			//   * DataAvailableToProcess -> a generator positively claimed an
			//     incomplete frame still arriving across reads.  This is the
			//     normal partial-frame deferral; await the next read quietly.
			//   * Anything else -> a non-conforming generator returned a
			//     "keep processing" code without consuming.  Looping would
			//     hang within this frame, so break and log the contract
			//     violation so the offending generator is diagnosable.
			if (error == make_error_code(ErrorCodes::Protocol_ErrorCodes::DataAvailableToProcess))
			{
				LogTrace(Channel::Protocol, "Identified frame incomplete; awaiting more serial data");
			}
			else
			{
				LogError(Channel::Protocol, [v = error.value()] { return std::format(
					"Generator returned a keep-processing code ({}) without consuming any bytes; breaking to avoid an infinite parse loop", v); });
			}
		}

		return false;
	}

	std::size_t ProtocolTask::ProcessMessages(ReadOps_SerialBufferType& circular_buffer)
	{
		std::size_t messages_parsed = 0;

		const auto profiler = Factory::ProfilerFactory::Instance().Get();

		// Keep trying to parse messages from the buffer until we need more data
		while (!circular_buffer.empty())
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ProtocolThread::ProcessMessages", std::source_location::current());

			auto msg_processing_start = std::chrono::steady_clock::now();

			// Snapshot the buffer size BEFORE generation so we can enforce the
			// consume-or-defer invariant: a "keep processing" error code must have
			// shrunk the buffer, otherwise the loop would spin forever within one
			// frame.  See MessageGeneratorRegistry::GenerateMessage for the contract.
			const std::size_t size_before = circular_buffer.size();

			auto message = MessageGeneratorRegistry::Instance().GenerateMessage(circular_buffer);
			if (message)
			{
				++messages_parsed;
				HandleParsedMessage(*message, msg_processing_start, profiler);
			}
			else if (HandleGenerationError(message.error(), circular_buffer, size_before))
			{
				continue;
			}
			else
			{
				break;
			}
		}

		return messages_parsed;
	}

}
// namespace AqualinkAutomate::Protocol
