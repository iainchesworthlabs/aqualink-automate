#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <boost/circular_buffer.hpp>
#include <boost/signals2.hpp>

#include "kernel/statistics_hub.h"
#include "profiling/types/profiling_types.h"
#include "protocol/protocol_handler_constants.h"
#include "protocol/protocol_handler_types.h"
#include "protocol/protocol_message_types.h"

namespace AqualinkAutomate::Protocol
{

	class ProtocolTask
	{
	public:
		// single_read_per_poll bounds the read loop to ONE serial chunk per Poll()
		// for capture replay (see --replay-filename).  The application's frame loop
		// owns the pacing (it steps at a fixed processing period); this bound just
		// stops a single Poll() from slurping the whole capture in one frame, which
		// would defeat pacing AND overflow the circular buffer (nothing is parsed
		// between back-to-back reads).  False (the default) drains everything
		// available each Poll() — correct for real ports (which self-pace via
		// would_block as the OS buffer empties) and for tests.
		ProtocolTask(std::shared_ptr<Serial::SerialPort> serial_port,
					 std::shared_ptr<Kernel::StatisticsHub> statistics_hub,
					 bool single_read_per_poll = false);
		~ProtocolTask();

		ProtocolTask(const ProtocolTask&) = delete;
		ProtocolTask& operator=(const ProtocolTask&) = delete;
		ProtocolTask(ProtocolTask&&) = delete;
		ProtocolTask& operator=(ProtocolTask&&) = delete;

		/// Advance the protocol task.  Returns true if work was performed
		/// (messages parsed or writes pending), signalling the caller that
		/// sleeping can be skipped for tighter response latency.
		[[nodiscard]] bool Poll();

		void EnqueueWrite(std::vector<uint8_t> buffer);

		template<typename MESSAGE_PUBLISHER>
		void ConnectWriteSignal()
		{
			if (auto publisher = MESSAGE_PUBLISHER::GetPublisher())
			{
				m_WriteSignalConnections.push_back(publisher->connect(
					[this](typename MESSAGE_PUBLISHER::PublisherRef payload)
					{
						std::vector<uint8_t> buffer;
						if (payload.get().Serialize(buffer) && !buffer.empty())
						{
							EnqueueWrite(std::move(buffer));
						}
					}
				));
			}
		}

	private:
		void DrainWrites();
		void DrainReads();
		std::size_t ProcessMessages(ReadOps_SerialBufferType& circular_buffer);

		// Success-branch body of the ProcessMessages loop: dispatch a decoded
		// message to its handlers (behind an exception barrier) and record the
		// per-message processing latency.
		void HandleParsedMessage(const ProtocolMessagePtr& message,
								 std::chrono::steady_clock::time_point msg_processing_start,
								 const Types::ProfilerTypePtr& profiler);

		// Error-branch body of the ProcessMessages loop.  Dispatches the error
		// and applies the consume-or-defer invariant.  Returns true if the loop
		// should CONTINUE parsing (forward progress on a keep-processing code),
		// false if it should BREAK (need more data or a contract violation).
		[[nodiscard]] bool HandleGenerationError(const ProtocolErrorCode& error,
												 const ReadOps_SerialBufferType& circular_buffer,
												 std::size_t size_before);

	private:
		std::shared_ptr<Serial::SerialPort> m_SerialPort;
		std::shared_ptr<Kernel::StatisticsHub> m_StatisticsHub;

		std::deque<std::vector<uint8_t>> m_WriteQueue;

		ReadOps_SerialBufferType m_SerialBuffer;
		std::array<uint8_t, Constants::SERIAL_READ_CHUNK_SIZE> m_ReadBuffer{};
		std::size_t m_WriteOffset{0};

		// When true, read at most one serial chunk per Poll() (see constructor).
		bool m_SingleReadPerPoll;

	private:
		std::vector<boost::signals2::scoped_connection> m_WriteSignalConnections;
	};

}
// namespace AqualinkAutomate::Protocol
