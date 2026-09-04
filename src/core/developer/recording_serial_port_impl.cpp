#include "developer/recording_serial_port_impl.h"

#include <filesystem>
#include <format>
#include <iomanip>
#include <system_error>

#include "platform/safe_ctime.h"

namespace AqualinkAutomate::Developer
{

	RecordingSerialPortImpl::RecordingSerialPortImpl(std::unique_ptr<Interfaces::ISerialPortImpl> wrapped_impl)
		: m_WrappedImpl(std::move(wrapped_impl))
		, m_StartTime(std::chrono::steady_clock::now())
	{
		// Pass-through by default: recording stays OFF until StartRecording() is
		// called (e.g. by the diagnostics route).  No file is opened here.
		LogDebug(Channel::Serial, "Serial recording decorator installed (recording is OFF)");
	}

	RecordingSerialPortImpl::RecordingSerialPortImpl(
		std::unique_ptr<Interfaces::ISerialPortImpl> wrapped_impl,
		const std::string& recording_file_path)
		: m_WrappedImpl(std::move(wrapped_impl))
		, m_StartTime(std::chrono::steady_clock::now())
	{
		// Start-at-boot path (preserves `--record-serial <file>`).
		RecordingSerialPortImpl::StartRecording(recording_file_path);
	}

	RecordingSerialPortImpl::~RecordingSerialPortImpl()
	{
		// Taking the mutex can raise std::system_error and CloseRecordingFile()
		// writes to / closes the stream and logs (std::format). A destructor must
		// not let any of that escape (cpp:S1048), so the teardown is guarded.
		try
		{
			std::lock_guard<std::mutex> lock(m_FileMutex);
			CloseRecordingFile();
		}
		catch (...)   // NOSONAR(cpp:S1181) — destructor boundary: nothing may escape a dtor (S1048)
		{
			// Swallow — nothing may escape a destructor.
		}
	}

	bool RecordingSerialPortImpl::StartRecording(const std::string& filename)
	{
		std::lock_guard<std::mutex> lock(m_FileMutex);

		if (m_RecordingEnabled.load())
		{
			LogWarning(Channel::Serial, std::format("Serial recording already in progress (file: {}); ignoring start request for {}", m_RecordingFilePath, filename));
			return false;
		}

		return OpenRecordingFile(filename);
	}

	bool RecordingSerialPortImpl::StopRecording()
	{
		std::lock_guard<std::mutex> lock(m_FileMutex);

		if (!m_RecordingEnabled.load())
		{
			LogDebug(Channel::Serial, "Stop recording requested but no recording is in progress");
			return false;
		}

		LogInfo(Channel::Serial, std::format("Stopping serial recording ({} bytes written to {})", m_BytesWritten, m_RecordingFilePath));
		CloseRecordingFile();
		return true;
	}

	bool RecordingSerialPortImpl::IsRecording() const
	{
		return m_RecordingEnabled.load();
	}

	Interfaces::IRecordingController::Status RecordingSerialPortImpl::RecordingStatus() const
	{
		std::lock_guard<std::mutex> lock(m_FileMutex);

		Interfaces::IRecordingController::Status status;
		status.recording = m_RecordingEnabled.load();
		status.file = m_RecordingFilePath;
		status.bytes_written = m_BytesWritten;
		return status;
	}

	bool RecordingSerialPortImpl::OpenRecordingFile(const std::string& recording_file_path)
	{
		// Caller holds m_FileMutex.

		// Defence in depth: the diagnostics HTTP route already jails the untrusted
		// web filename to a fixed capture directory, but this opener is also reached
		// from `--record-serial <file>` and the start-at-boot ctor.  Reject any path
		// that contains a parent-directory ("..") component so a traversal value can
		// never reach the truncating open() below.  Inspect the RAW components (not
		// the lexically-normalised form) so a "tmp/../x" cannot be silently
		// collapsed past this guard.
		for (const auto& component : std::filesystem::path{ recording_file_path })
		{
			if (component == "..")
			{
				LogError(Channel::Serial, std::format("Refusing to record to a path containing '..': {}", recording_file_path));
				m_RecordingFilePath.clear();
				return false;
			}
		}

		LogInfo(Channel::Serial, std::format("Serial recording enabled, output file: {}", recording_file_path));

		m_RecordingFile.open(recording_file_path, std::ios::out | std::ios::trunc);
		if (!m_RecordingFile.is_open())
		{
			LogError(Channel::Serial, std::format("Failed to open serial recording file: {}", recording_file_path));
			m_RecordingFilePath.clear();
			return false;
		}

		// Restrict the capture to the owner (0600).  It contains raw RS-485 bus
		// traffic that can reveal live system state, and ofstream creates the file
		// with the process umask (commonly world-readable 0644).  Best-effort: on
		// Windows std::filesystem maps this to the closest ACL/no-op.
		{
			std::error_code perm_ec;
			std::filesystem::permissions(recording_file_path,
				std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
				std::filesystem::perm_options::replace, perm_ec);
			if (perm_ec)
			{
				LogDebug(Channel::Serial, std::format("Could not restrict permissions on recording file {}: {}", recording_file_path, perm_ec.message()));
			}
		}

		m_RecordingFilePath = recording_file_path;
		m_BytesWritten = 0;
		m_StartTime = std::chrono::steady_clock::now();

		// Write header with timestamp.
		auto now = std::chrono::system_clock::now();
		auto time_t_now = std::chrono::system_clock::to_time_t(now);
		char time_buf[26]{};
		Platform::SafeCtime(&time_t_now, time_buf, sizeof(time_buf));
		m_RecordingFile << "# Serial recording started at: " << time_buf;
		m_RecordingFile << "# Format: [timestamp_ms] direction byte|byte|byte|...\n";
		m_RecordingFile << "# Direction: R=read (from device), W=write (to device)\n";
		m_RecordingFile << "#\n";
		m_RecordingFile.flush();

		// Publish "on" only AFTER the file and header are ready, so the read/write
		// fast path never sees recording-enabled with an unwritten header.
		m_RecordingEnabled.store(true);

		LogDebug(Channel::Serial, "Serial recording file opened successfully");
		return true;
	}

	void RecordingSerialPortImpl::CloseRecordingFile()
	{
		// Caller holds m_FileMutex.

		// Stop the fast path BEFORE touching the stream so an in-flight reader/
		// writer that already passed the gate finishes under the lock we hold.
		m_RecordingEnabled.store(false);

		if (m_RecordingFile.is_open())
		{
			m_RecordingFile << "# Recording ended\n";
			m_RecordingFile.close();
			LogInfo(Channel::Serial, std::format("Serial recording file closed: {}", m_RecordingFilePath));
		}

		m_RecordingFilePath.clear();
	}

	void RecordingSerialPortImpl::open(const std::string& device_name)
	{
		m_WrappedImpl->open(device_name);

		if (m_RecordingEnabled.load())
		{
			std::lock_guard<std::mutex> lock(m_FileMutex);
			if (m_RecordingFile.is_open())
			{
				WriteTimestamp();
				m_RecordingFile << "# Opened device: " << device_name << "\n";
				m_RecordingFile.flush();
			}
		}
	}

	void RecordingSerialPortImpl::open(const std::string& device_name, boost::system::error_code& ec)
	{
		m_WrappedImpl->open(device_name, ec);

		if (m_RecordingEnabled.load() && !ec)
		{
			std::lock_guard<std::mutex> lock(m_FileMutex);
			if (m_RecordingFile.is_open())
			{
				WriteTimestamp();
				m_RecordingFile << "# Opened device: " << device_name << "\n";
				m_RecordingFile.flush();
			}
		}
	}

	bool RecordingSerialPortImpl::is_open() const
	{
		return m_WrappedImpl->is_open();
	}

	void RecordingSerialPortImpl::cancel()
	{
		m_WrappedImpl->cancel();
	}

	void RecordingSerialPortImpl::cancel(boost::system::error_code& ec)
	{
		m_WrappedImpl->cancel(ec);
	}

	void RecordingSerialPortImpl::close()
	{
		m_WrappedImpl->close();

		if (m_RecordingEnabled.load())
		{
			std::lock_guard<std::mutex> lock(m_FileMutex);
			if (m_RecordingFile.is_open())
			{
				WriteTimestamp();
				m_RecordingFile << "# Device closed\n";
				m_RecordingFile.flush();
			}
		}
	}

	void RecordingSerialPortImpl::close(boost::system::error_code& ec)
	{
		m_WrappedImpl->close(ec);

		if (m_RecordingEnabled.load() && !ec)
		{
			std::lock_guard<std::mutex> lock(m_FileMutex);
			if (m_RecordingFile.is_open())
			{
				WriteTimestamp();
				m_RecordingFile << "# Device closed\n";
				m_RecordingFile.flush();
			}
		}
	}

	void RecordingSerialPortImpl::set_baud_rate(uint32_t rate, boost::system::error_code& ec)
	{
		m_WrappedImpl->set_baud_rate(rate, ec);

		if (m_RecordingEnabled.load() && !ec)
		{
			std::lock_guard<std::mutex> lock(m_FileMutex);
			if (m_RecordingFile.is_open())
			{
				WriteTimestamp();
				m_RecordingFile << "# Set baud rate: " << rate << "\n";
				m_RecordingFile.flush();
			}
		}
	}

	void RecordingSerialPortImpl::set_character_size(uint8_t bits, boost::system::error_code& ec)
	{
		m_WrappedImpl->set_character_size(bits, ec);

		if (m_RecordingEnabled.load() && !ec)
		{
			std::lock_guard<std::mutex> lock(m_FileMutex);
			if (m_RecordingFile.is_open())
			{
				WriteTimestamp();
				m_RecordingFile << "# Set character size: " << static_cast<int>(bits) << "\n";
				m_RecordingFile.flush();
			}
		}
	}

	void RecordingSerialPortImpl::set_flow_control(Serial::FlowControl fc, boost::system::error_code& ec)
	{
		m_WrappedImpl->set_flow_control(fc, ec);
	}

	void RecordingSerialPortImpl::set_parity(Serial::Parity p, boost::system::error_code& ec)
	{
		m_WrappedImpl->set_parity(p, ec);
	}

	void RecordingSerialPortImpl::set_stop_bits(Serial::StopBits sb, boost::system::error_code& ec)
	{
		m_WrappedImpl->set_stop_bits(sb, ec);
	}

	void RecordingSerialPortImpl::set_read_timeout(std::chrono::milliseconds timeout, boost::system::error_code& ec)
	{
		m_WrappedImpl->set_read_timeout(timeout, ec);
	}

	std::size_t RecordingSerialPortImpl::read_some(const boost::asio::mutable_buffer& buffer)
	{
		auto bytes_read = m_WrappedImpl->read_some(buffer);

		if (m_RecordingEnabled.load() && bytes_read > 0)
		{
			RecordReadData(static_cast<const uint8_t*>(buffer.data()), bytes_read);
		}

		return bytes_read;
	}

	std::size_t RecordingSerialPortImpl::read_some(const boost::asio::mutable_buffer& buffer, boost::system::error_code& ec)
	{
		auto bytes_read = m_WrappedImpl->read_some(buffer, ec);

		if (m_RecordingEnabled.load() && bytes_read > 0 && !ec)
		{
			RecordReadData(static_cast<const uint8_t*>(buffer.data()), bytes_read);
		}

		return bytes_read;
	}

	std::size_t RecordingSerialPortImpl::write_some(const boost::asio::const_buffer& buffer)
	{
		if (m_RecordingEnabled.load() && buffer.size() > 0)
		{
			RecordWriteData(static_cast<const uint8_t*>(buffer.data()), buffer.size());
		}

		return m_WrappedImpl->write_some(buffer);
	}

	std::size_t RecordingSerialPortImpl::write_some(const boost::asio::const_buffer& buffer, boost::system::error_code& ec)
	{
		if (m_RecordingEnabled.load() && buffer.size() > 0)
		{
			RecordWriteData(static_cast<const uint8_t*>(buffer.data()), buffer.size());
		}

		return m_WrappedImpl->write_some(buffer, ec);
	}

	void RecordingSerialPortImpl::RecordReadData(const uint8_t* data, std::size_t length)
	{
		RecordDataLine('R', data, length);
	}

	void RecordingSerialPortImpl::RecordWriteData(const uint8_t* data, std::size_t length)
	{
		RecordDataLine('W', data, length);
	}

	void RecordingSerialPortImpl::RecordDataLine(char direction, const uint8_t* data, std::size_t length)
	{
		// Disk-fill DoS guard: a recording can be started remotely (the diagnostics
		// route) and otherwise grows without bound, so a single request could exhaust
		// the disk and degrade or crash the daemon. Cap the capture and auto-stop once
		// it is reached. 256 MiB is far larger than any legitimate diagnostic capture.
		constexpr std::size_t MAX_RECORDING_BYTES = static_cast<std::size_t>(256) * 1024 * 1024;

		std::lock_guard<std::mutex> lock(m_FileMutex);

		// Recording may have been stopped between the fast-path gate check and
		// acquiring the lock; re-check the open file before writing.
		if (!m_RecordingFile.is_open())
		{
			return;
		}

		if (m_BytesWritten >= MAX_RECORDING_BYTES)
		{
			LogWarning(Channel::Serial, std::format(
				"Serial recording reached the {}-byte cap ({} bytes written to '{}'); auto-stopping",
				MAX_RECORDING_BYTES, m_BytesWritten, m_RecordingFilePath));
			CloseRecordingFile();   // safe: we hold m_FileMutex (as StopRecording does)
			return;
		}

		WriteTimestamp();
		m_RecordingFile << direction << " ";

		for (std::size_t i = 0; i < length; ++i)
		{
			m_RecordingFile << "0x" << std::hex << std::setw(2) << std::setfill('0')
				<< static_cast<int>(data[i]) << std::dec;
			if (i < length - 1)
			{
				m_RecordingFile << "|";
			}
		}
		m_RecordingFile << "\n";
		m_RecordingFile.flush();

		m_BytesWritten += length;
	}

	void RecordingSerialPortImpl::WriteTimestamp()
	{
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_StartTime);
		m_RecordingFile << "[" << std::setw(10) << elapsed.count() << "] ";
	}

}
// namespace AqualinkAutomate::Developer
