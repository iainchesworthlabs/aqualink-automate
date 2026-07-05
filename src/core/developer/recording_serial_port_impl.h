#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include "interfaces/irecordingcontroller.h"
#include "interfaces/iserialportimpl.h"
#include "logging/logging.h"
#include "serial/serial_port_enums.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Developer
{

	/// @brief Decorator that wraps any ISerialPortImpl and can record serial data
	///        to a file in the MockSerialPortImpl replay format (0x##|0x##|...).
	///
	/// Recording is RUNTIME-CONTROLLABLE: the decorator can sit permanently in the
	/// production serial chain (physical or remote) defaulting to OFF, and a
	/// diagnostics route can toggle it via the IRecordingController interface to
	/// capture a replayable fixture from live hardware on demand.
	///
	/// When NOT recording the decorator is a transparent pass-through: a single
	/// relaxed atomic load gates the per-read/write fast path, no file is held
	/// open, and no per-byte work is performed.
	///
	/// Recording includes timestamps to help with debugging timing-related issues.
	class RecordingSerialPortImpl : public Interfaces::ISerialPortImpl, public Interfaces::IRecordingController
	{
	public:
		/// @brief Wrap an existing serial port WITHOUT starting recording.
		///
		/// Use this in the production chain: the decorator is a pass-through until
		/// StartRecording() is called (e.g. by the diagnostics route).
		/// @param wrapped_impl The underlying serial port implementation to wrap.
		explicit RecordingSerialPortImpl(std::unique_ptr<Interfaces::ISerialPortImpl> wrapped_impl);

		/// @brief Wrap an existing serial port AND start recording immediately.
		///
		/// Preserves the startup `--record-serial <file>` behaviour (record from
		/// boot).  Equivalent to the pass-through constructor followed by
		/// StartRecording(recording_file_path).
		/// @param wrapped_impl The underlying serial port implementation to wrap.
		/// @param recording_file_path Path to the file where serial data is recorded.
		RecordingSerialPortImpl(std::unique_ptr<Interfaces::ISerialPortImpl> wrapped_impl, const std::string& recording_file_path);

		~RecordingSerialPortImpl() override;

		RecordingSerialPortImpl(const RecordingSerialPortImpl&) = delete;
		RecordingSerialPortImpl& operator=(const RecordingSerialPortImpl&) = delete;
		RecordingSerialPortImpl(RecordingSerialPortImpl&& other) noexcept = delete;
		RecordingSerialPortImpl& operator=(RecordingSerialPortImpl&& other) noexcept = delete;

		// Interfaces::IRecordingController
		bool StartRecording(const std::string& filename) override;
		bool StopRecording() override;
		bool IsRecording() const override;
		Interfaces::IRecordingController::Status RecordingStatus() const override;

		// Interfaces::ISerialPortImpl
		void open(const std::string& device_name) override;
		void open(const std::string& device_name, boost::system::error_code& ec) override;
		bool is_open() const override;
		void cancel() override;
		void cancel(boost::system::error_code& ec) override;
		void close() override;
		void close(boost::system::error_code& ec) override;

		void set_baud_rate(uint32_t rate, boost::system::error_code& ec) override;
		void set_character_size(uint8_t bits, boost::system::error_code& ec) override;
		void set_flow_control(Serial::FlowControl fc, boost::system::error_code& ec) override;
		void set_parity(Serial::Parity p, boost::system::error_code& ec) override;
		void set_stop_bits(Serial::StopBits sb, boost::system::error_code& ec) override;
		void set_read_timeout(std::chrono::milliseconds timeout, boost::system::error_code& ec) override;

		std::size_t read_some(const boost::asio::mutable_buffer& buffer) override;
		std::size_t read_some(const boost::asio::mutable_buffer& buffer, boost::system::error_code& ec) override;
		std::size_t write_some(const boost::asio::const_buffer& buffer) override;
		std::size_t write_some(const boost::asio::const_buffer& buffer, boost::system::error_code& ec) override;

	private:
		void RecordReadData(const uint8_t* data, std::size_t length);
		void RecordWriteData(const uint8_t* data, std::size_t length);
		void RecordDataLine(char direction, const uint8_t* data, std::size_t length);
		void WriteTimestamp();

		// Open the capture file and write the header. Caller must hold m_FileMutex.
		// Returns true on success (and flips m_RecordingEnabled on).
		bool OpenRecordingFile(const std::string& recording_file_path);
		// Close the active capture file. Caller must hold m_FileMutex.
		void CloseRecordingFile();

	private:
		std::unique_ptr<Interfaces::ISerialPortImpl> m_WrappedImpl;

		// Lock-free fast-path gate.  Read on every read_some/write_some; toggled
		// under m_FileMutex by Start/StopRecording.  When false the decorator does
		// zero recording work.
		std::atomic<bool> m_RecordingEnabled{ false };

		mutable std::mutex m_FileMutex;
		std::string m_RecordingFilePath;
		std::ofstream m_RecordingFile;
		std::chrono::steady_clock::time_point m_StartTime;
		std::size_t m_BytesWritten{ 0 };
	};

}
// namespace AqualinkAutomate::Developer
