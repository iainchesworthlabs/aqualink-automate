#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>
#include <boost/test/unit_test.hpp>

#include "developer/recording_serial_port_impl.h"
#include "interfaces/irecordingcontroller.h"

#include "mocks/mock_testserialportimpl.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Developer;

//=============================================================================
// RecordingSerialPortImpl decorator: the pass-through / annotation branches of
// every ISerialPortImpl overload, the IRecordingController state machine, and
// the file-open failure path.  The wrapped port is the queue-driven
// TestSerialPortImpl so reads/writes are fully deterministic.
//=============================================================================

namespace
{
	struct TempRecordingPath
	{
		std::filesystem::path path;

		TempRecordingPath()
		{
			static int counter = 0;
			path = std::filesystem::temp_directory_path() /
				std::filesystem::path(std::string("aa_rec_impl_") + std::to_string(++counter) + ".cap");
			std::error_code ec;
			std::filesystem::remove(path, ec);
		}

		~TempRecordingPath()
		{
			std::error_code ec;
			std::filesystem::remove(path, ec);
		}

		TempRecordingPath(const TempRecordingPath&) = delete;
		TempRecordingPath& operator=(const TempRecordingPath&) = delete;
	};

	std::string ReadAll(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::ostringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	std::size_t CountOf(const std::string& haystack, const std::string& needle)
	{
		std::size_t count = 0;
		for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
		{
			++count;
		}
		return count;
	}

	// Build a decorator around a fresh TestSerialPortImpl; hand back both.
	struct Wrapped
	{
		Test::TestSerialPortImpl* inner{ nullptr };
		std::unique_ptr<RecordingSerialPortImpl> recorder;

		explicit Wrapped(bool start_recording = false, const std::string& file = {})
		{
			auto impl = std::make_unique<Test::TestSerialPortImpl>();
			inner = impl.get();
			inner->EnableTestMode(true);

			if (start_recording)
			{
				recorder = std::make_unique<RecordingSerialPortImpl>(std::move(impl), file);
			}
			else
			{
				recorder = std::make_unique<RecordingSerialPortImpl>(std::move(impl));
			}
		}
	};
}
// anonymous namespace

BOOST_AUTO_TEST_SUITE(RecordingSerialPortImpl_TestSuite)

// -----------------------------------------------------------------------------
// Start / stop state machine
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(StartRecording_Twice_SecondRequestIsRefused)
{
	TempRecordingPath first;
	TempRecordingPath second;
	Wrapped w;

	BOOST_REQUIRE(w.recorder->StartRecording(first.path.string()));
	BOOST_CHECK(w.recorder->IsRecording());

	// A second start while active is ignored; the ORIGINAL file stays active.
	BOOST_CHECK(!w.recorder->StartRecording(second.path.string()));
	BOOST_CHECK(w.recorder->IsRecording());
	BOOST_CHECK_EQUAL(w.recorder->RecordingStatus().file, first.path.string());
	BOOST_CHECK(!std::filesystem::exists(second.path));

	BOOST_CHECK(w.recorder->StopRecording());
	BOOST_CHECK(!w.recorder->IsRecording());

	// Stopping again is a no-op that reports "nothing was recording".
	BOOST_CHECK(!w.recorder->StopRecording());

	const auto status = w.recorder->RecordingStatus();
	BOOST_CHECK(!status.recording);
	BOOST_CHECK(status.file.empty());
}

BOOST_AUTO_TEST_CASE(StartRecording_UnopenableFile_ReturnsFalseAndStaysOff)
{
	// A directory cannot be opened for writing: the opener must fail cleanly
	// and leave the decorator as a pass-through.
	Wrapped w;
	const auto directory = std::filesystem::temp_directory_path().string();

	BOOST_CHECK(!w.recorder->StartRecording(directory));
	BOOST_CHECK(!w.recorder->IsRecording());
	BOOST_CHECK(w.recorder->RecordingStatus().file.empty());

	// Still a working pass-through afterwards.
	w.inner->QueueReadData({ 0x10, 0x02 });
	std::vector<uint8_t> buffer(8);
	boost::system::error_code ec;
	BOOST_CHECK_EQUAL(w.recorder->read_some(boost::asio::buffer(buffer), ec), 2u);
	BOOST_CHECK(!ec);
}

BOOST_AUTO_TEST_CASE(StartAtBootConstructor_RecordsImmediately)
{
	TempRecordingPath rec;
	Wrapped w(/*start_recording=*/true, rec.path.string());

	BOOST_CHECK(w.recorder->IsRecording());
	BOOST_CHECK_EQUAL(w.recorder->RecordingStatus().file, rec.path.string());
	BOOST_REQUIRE(std::filesystem::exists(rec.path));

	const auto header = ReadAll(rec.path);
	BOOST_CHECK(header.find("# Serial recording started at:") != std::string::npos);
	BOOST_CHECK(header.find("# Direction: R=read") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Annotation lines written by the lifecycle overloads while recording
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ThrowingOpenAndClose_AnnotateCaptureWhileRecording)
{
	TempRecordingPath rec;
	Wrapped w;

	// The wrapped test port is opened by its constructor; close it (throwing
	// overload, no error expected) so the subsequent throwing open succeeds.
	BOOST_REQUIRE(w.recorder->StartRecording(rec.path.string()));

	BOOST_CHECK_NO_THROW(w.recorder->close());
	BOOST_CHECK(!w.recorder->is_open());

	BOOST_CHECK_NO_THROW(w.recorder->open("wrapped_device"));
	BOOST_CHECK(w.recorder->is_open());

	BOOST_REQUIRE(w.recorder->StopRecording());

	const auto text = ReadAll(rec.path);
	BOOST_CHECK_EQUAL(CountOf(text, "# Device closed"), 1u);
	BOOST_CHECK_EQUAL(CountOf(text, "# Opened device: wrapped_device"), 1u);
	BOOST_CHECK_EQUAL(CountOf(text, "# Recording ended"), 1u);
}

BOOST_AUTO_TEST_CASE(ErrorCodeOpen_FailureIsNotAnnotated_SuccessIs)
{
	TempRecordingPath rec;
	Wrapped w;
	BOOST_REQUIRE(w.recorder->StartRecording(rec.path.string()));

	// Already open -> the wrapped port reports already_open; no annotation.
	boost::system::error_code ec;
	w.recorder->open("again", ec);
	BOOST_CHECK(ec == boost::asio::error::already_open);

	// Close (ec overload) then re-open (ec overload): both annotated.
	w.recorder->close(ec);
	BOOST_CHECK(!ec);
	w.recorder->open("reopened", ec);
	BOOST_CHECK(!ec);

	BOOST_REQUIRE(w.recorder->StopRecording());

	const auto text = ReadAll(rec.path);
	BOOST_CHECK_EQUAL(CountOf(text, "# Opened device: again"), 0u);
	BOOST_CHECK_EQUAL(CountOf(text, "# Opened device: reopened"), 1u);
	BOOST_CHECK_EQUAL(CountOf(text, "# Device closed"), 1u);
}

BOOST_AUTO_TEST_CASE(LifecycleOverloads_WhenNotRecording_ArePureDelegation)
{
	TempRecordingPath rec;
	Wrapped w;

	// Never started: no file must ever be created by any overload.
	boost::system::error_code ec;
	w.recorder->close(ec);
	w.recorder->open("dev", ec);
	BOOST_CHECK(!ec);
	w.recorder->close();
	w.recorder->open("dev2");
	w.recorder->set_baud_rate(9600, ec);
	w.recorder->set_character_size(8, ec);
	BOOST_CHECK(!ec);
	BOOST_CHECK(w.recorder->is_open());
	BOOST_CHECK(!std::filesystem::exists(rec.path));
}

BOOST_AUTO_TEST_CASE(PortSettings_AnnotateBaudAndCharacterSize_DelegateTheRest)
{
	TempRecordingPath rec;
	Wrapped w;
	BOOST_REQUIRE(w.recorder->StartRecording(rec.path.string()));

	boost::system::error_code ec = boost::asio::error::fault;   // must be cleared by the wrapped port
	w.recorder->set_baud_rate(19200, ec);
	BOOST_CHECK(!ec);

	ec = boost::asio::error::fault;
	w.recorder->set_character_size(7, ec);
	BOOST_CHECK(!ec);

	ec = boost::asio::error::fault;
	w.recorder->set_flow_control(Serial::FlowControl::None, ec);
	BOOST_CHECK(!ec);
	ec = boost::asio::error::fault;
	w.recorder->set_parity(Serial::Parity::Even, ec);
	BOOST_CHECK(!ec);
	ec = boost::asio::error::fault;
	w.recorder->set_stop_bits(Serial::StopBits::Two, ec);
	BOOST_CHECK(!ec);
	ec = boost::asio::error::fault;
	w.recorder->set_read_timeout(std::chrono::milliseconds(250), ec);
	BOOST_CHECK(!ec);

	// cancel() overloads delegate straight through (the wrapped port is open).
	BOOST_CHECK_NO_THROW(w.recorder->cancel());
	ec = boost::asio::error::fault;
	w.recorder->cancel(ec);
	BOOST_CHECK(!ec);

	BOOST_REQUIRE(w.recorder->StopRecording());

	const auto text = ReadAll(rec.path);
	BOOST_CHECK_EQUAL(CountOf(text, "# Set baud rate: 19200"), 1u);
	BOOST_CHECK_EQUAL(CountOf(text, "# Set character size: 7"), 1u);
}

// -----------------------------------------------------------------------------
// Data path: both read/write overloads while recording
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ThrowingReadAndWrite_RecordBytesInRecorderFormat)
{
	TempRecordingPath rec;
	Wrapped w;
	BOOST_REQUIRE(w.recorder->StartRecording(rec.path.string()));

	w.inner->QueueReadData({ 0x10, 0x02, 0xFF });
	std::vector<uint8_t> buffer(16);
	BOOST_CHECK_EQUAL(w.recorder->read_some(boost::asio::buffer(buffer)), 3u);

	const std::vector<uint8_t> out{ 0xAB, 0xCD };
	w.inner->QueueWriteResponse(out.size());
	BOOST_CHECK_EQUAL(w.recorder->write_some(boost::asio::buffer(out)), 2u);
	BOOST_CHECK_EQUAL_COLLECTIONS(w.inner->GetWrittenData().begin(), w.inner->GetWrittenData().end(), out.begin(), out.end());

	const auto status = w.recorder->RecordingStatus();
	BOOST_CHECK(status.recording);
	BOOST_CHECK_EQUAL(status.bytes_written, 5u);

	BOOST_REQUIRE(w.recorder->StopRecording());

	const auto text = ReadAll(rec.path);
	BOOST_CHECK(text.find("R 0x10|0x02|0xff") != std::string::npos);
	BOOST_CHECK(text.find("W 0xab|0xcd") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ErrorCodeRead_ErrorOrZeroBytes_NotRecorded)
{
	TempRecordingPath rec;
	Wrapped w;
	BOOST_REQUIRE(w.recorder->StartRecording(rec.path.string()));

	std::vector<uint8_t> buffer(16);
	boost::system::error_code ec;

	// A read error yields nothing to record.
	w.inner->QueueReadError(boost::asio::error::operation_aborted);
	BOOST_CHECK_EQUAL(w.recorder->read_some(boost::asio::buffer(buffer), ec), 0u);
	BOOST_CHECK(ec == boost::asio::error::operation_aborted);

	// A zero-length write yields nothing to record either.
	const std::vector<uint8_t> empty;
	w.inner->QueueWriteResponse(0);
	BOOST_CHECK_EQUAL(w.recorder->write_some(boost::asio::buffer(empty), ec), 0u);

	BOOST_CHECK_EQUAL(w.recorder->RecordingStatus().bytes_written, 0u);
	BOOST_REQUIRE(w.recorder->StopRecording());

	const auto text = ReadAll(rec.path);
	BOOST_CHECK_EQUAL(CountOf(text, "] R "), 0u);
	BOOST_CHECK_EQUAL(CountOf(text, "] W "), 0u);
}

BOOST_AUTO_TEST_CASE(ThrowingReadAndWrite_WhenNotRecording_DelegateOnly)
{
	TempRecordingPath rec;
	Wrapped w;

	w.inner->QueueReadData({ 0x01 });
	std::vector<uint8_t> buffer(4);
	BOOST_CHECK_EQUAL(w.recorder->read_some(boost::asio::buffer(buffer)), 1u);

	const std::vector<uint8_t> out{ 0x02, 0x03 };
	w.inner->QueueWriteResponse(out.size());
	BOOST_CHECK_EQUAL(w.recorder->write_some(boost::asio::buffer(out)), 2u);

	BOOST_CHECK(!std::filesystem::exists(rec.path));
	BOOST_CHECK_EQUAL(w.recorder->RecordingStatus().bytes_written, 0u);
}

// -----------------------------------------------------------------------------
// Destruction while recording closes the capture cleanly
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Destructor_WhileRecording_ClosesFileWithFooter)
{
	TempRecordingPath rec;
	{
		Wrapped w;
		BOOST_REQUIRE(w.recorder->StartRecording(rec.path.string()));
		w.inner->QueueReadData({ 0x55 });
		std::vector<uint8_t> buffer(4);
		boost::system::error_code ec;
		BOOST_CHECK_EQUAL(w.recorder->read_some(boost::asio::buffer(buffer), ec), 1u);
		// No StopRecording(): the destructor must finalise the capture.
	}

	const auto text = ReadAll(rec.path);
	BOOST_CHECK(text.find("R 0x55") != std::string::npos);
	BOOST_CHECK_EQUAL(CountOf(text, "# Recording ended"), 1u);
}

BOOST_AUTO_TEST_SUITE_END()
