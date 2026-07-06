//
// Protocol fuzz SMOKE / regression test.
//
// The coverage-guided libFuzzer harnesses (fuzz/) require a Clang/libFuzzer build.
// This test exercises exactly the same untrusted-input decode paths those harnesses
// drive — the Jandy and Pentair generators + message factories + every registered
// DeserializeContents — but from inside the normal (MSVC/GCC/Clang) unit-test
// binary, using:
//
//   * the shared fuzz frame builders (fuzz/fuzz_frame_builders.h), asserting a
//     built frame round-trips (valid frame -> factory -> a message),
//   * a deterministic pseudo-random corpus of malformed frames and payloads, and
//   * the real recorded .cap wire frames (LoadFixtureFrames).
//
// Under a debug STL (MSVC _ITERATOR_DEBUG_LEVEL, libstdc++ _GLIBCXX_ASSERTIONS) an
// out-of-bounds span/vector access aborts, so this is a genuine regression guard:
// if a decoder is ever made unsafe, this test crashes here rather than in the field.
// It also guards the fuzz frame builders themselves so the harnesses cannot silently
// rot. NEVER weaken a decoder to make this pass — fix the decoder.
//

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <boost/circular_buffer.hpp>
#include <boost/test/unit_test.hpp>

#include <magic_enum/magic_enum.hpp>

#include "factories/jandy_message_factory.h"
#include "factories/jandy_message_factory_registration.h"
#include "generator/jandy_message_generator.h"
#include "messages/jandy_message_ids.h"

#include "factories/pentair_message_factory.h"
#include "generator/pentair_message_generator.h"
#include "messages/pentair_message_ids.h"

#include "logging/logging_severity_filter.h"

#include "fuzz_frame_builders.h"

#include "support/unit_test_loadfixture.h"

using namespace AqualinkAutomate;

namespace
{

	// Push a byte span through the Jandy protocol read engine (one decode attempt).
	void DriveJandyGenerator(std::span<const uint8_t> bytes)
	{
		boost::circular_buffer<uint8_t> buffer(bytes.size() + 8U);
		buffer.insert(buffer.end(), bytes.begin(), bytes.end());
		buffer.linearize();
		(void)Generators::GenerateMessageFromRawData(buffer);
	}

	void DrivePentairGenerator(std::span<const uint8_t> bytes)
	{
		boost::circular_buffer<uint8_t> buffer(bytes.size() + 8U);
		buffer.insert(buffer.end(), bytes.begin(), bytes.end());
		buffer.linearize();
		(void)Pentair::Generators::GenerateMessageFromRawData(buffer);
	}

	// Mirror the fuzz harnesses' LLVMFuzzerTestOneInput: feed the raw bytes into each
	// generator, and (when long enough) wrap a mutated payload in a valid frame and
	// push it through both factories.  Any OOB read / assertion here is a real bug.
	void DriveOneInput(std::span<const uint8_t> input)
	{
		DriveJandyGenerator(input);
		DrivePentairGenerator(input);

		if (input.size() >= 2U)
		{
			const auto frame = Fuzzing::BuildJandyFrame(input[0], input[1], input.subspan(2U));
			(void)Factory::JandyMessageFactoryT::CreateFromSerialData(frame);
			DriveJandyGenerator(frame);
		}

		if (input.size() >= 3U)
		{
			const auto frame = Fuzzing::BuildPentairFrame(input[0], input[1], input[2], input.subspan(3U));
			(void)Pentair::Factory::PentairMessageFactory::CreateFromSerialData(Fuzzing::PentairChecksummedRegion(frame));
			DrivePentairGenerator(frame);
		}
	}

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_ProtocolFuzzSmoke)

// A frame built by the fuzz builders must be a genuine, decodable wire frame — this
// is what lets the harness reach the per-type deserialisers past framing + checksum.
BOOST_AUTO_TEST_CASE(BuiltFramesRoundTripThroughFactories)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);

	// Jandy: an Ack (registered, decodes an empty payload) must round-trip.
	{
		const auto ack_type = static_cast<uint8_t>(magic_enum::enum_integer(Messages::JandyMessageIds::Ack));
		const std::vector<uint8_t> payload{ 0x00, 0x00 };
		const auto frame = Fuzzing::BuildJandyFrame(0x00, ack_type, payload);

		const auto result = Factory::JandyMessageFactoryT::CreateFromSerialData(frame);
		BOOST_TEST(result.has_value());
	}

	// Pentair: a Pump_Status with a full 8-byte DATA section must round-trip through
	// both the checksummed-region factory and the full-frame generator.
	{
		const auto cmd = static_cast<uint8_t>(magic_enum::enum_integer(Pentair::Messages::PentairMessageIds::Pump_Status));
		const std::vector<uint8_t> data{ 0x0A, 0x02, 0x00, 0x00, 0x09, 0x60, 0x00, 0x00 };
		const auto frame = Fuzzing::BuildPentairFrame(0x60, 0x10, cmd, data);

		const auto region = Fuzzing::PentairChecksummedRegion(frame);
		const auto result = Pentair::Factory::PentairMessageFactory::CreateFromSerialData(region);
		BOOST_TEST(result.has_value());

		boost::circular_buffer<uint8_t> buffer(frame.size() + 8U);
		buffer.insert(buffer.end(), frame.begin(), frame.end());
		buffer.linearize();
		const auto gen_result = Pentair::Generators::GenerateMessageFromRawData(buffer);
		BOOST_TEST(gen_result.has_value());
	}
}

// Every message-type / command byte, at several payload lengths, must decode without
// crashing — this walks the whole registered factory dispatch, hitting each concrete
// DeserializeContents with short/edge-length payloads.
BOOST_AUTO_TEST_CASE(EveryTypeAndCommandSurvivesEdgeLengthPayloads)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);

	const std::vector<std::size_t> payload_lengths{ 0U, 1U, 2U, 3U, 4U, 7U, 15U, 16U, 32U, 120U };

	std::size_t decoded_attempts = 0;
	for (int type = 0; type <= 0xFF; ++type)
	{
		for (const std::size_t len : payload_lengths)
		{
			const std::vector<uint8_t> payload(len, static_cast<uint8_t>(0xA5));

			const auto jandy_frame = Fuzzing::BuildJandyFrame(0x33, static_cast<uint8_t>(type), payload);
			(void)Factory::JandyMessageFactoryT::CreateFromSerialData(jandy_frame);
			DriveJandyGenerator(jandy_frame);

			const auto pentair_frame = Fuzzing::BuildPentairFrame(0x60, 0x10, static_cast<uint8_t>(type), payload);
			(void)Pentair::Factory::PentairMessageFactory::CreateFromSerialData(Fuzzing::PentairChecksummedRegion(pentair_frame));
			DrivePentairGenerator(pentair_frame);

			++decoded_attempts;
		}
	}

	// Reached here without an OOB abort; sanity-check the sweep actually ran.
	BOOST_TEST(decoded_attempts == 256U * payload_lengths.size());
}

// A deterministic pseudo-random corpus of arbitrary byte buffers must not crash any
// decode path. Fixed seed => reproducible; the size window spans the sub-minimum,
// typical and over-maximum packet ranges.
BOOST_AUTO_TEST_CASE(DeterministicRandomInputsDoNotCrash)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);

	std::mt19937_64 rng(0xA5A511C0FFEEULL);
	std::uniform_int_distribution<int> byte_dist(0, 255);
	std::uniform_int_distribution<int> size_dist(0, 160);

	constexpr int ITERATIONS = 20000;
	std::size_t processed = 0;
	for (int i = 0; i < ITERATIONS; ++i)
	{
		std::vector<uint8_t> input(static_cast<std::size_t>(size_dist(rng)));
		for (auto& b : input)
		{
			b = static_cast<uint8_t>(byte_dist(rng));
		}

		// Bias a fraction toward valid framing bytes so the generators' happy-path
		// scanning is exercised, not just the "no packet start" reject.
		if ((i % 4) == 0 && input.size() >= 4U)
		{
			input[0] = 0x10; input[1] = 0x02; // Jandy DLE/STX
		}
		else if ((i % 4) == 1 && input.size() >= 4U)
		{
			input[0] = 0xFF; input[1] = 0x00; input[2] = 0xFF; input[3] = 0xA5; // Pentair preamble
		}

		DriveOneInput(input);
		++processed;
	}

	BOOST_TEST(processed == static_cast<std::size_t>(ITERATIONS));
}

// The real recorded wire captures must decode cleanly frame-by-frame, and each frame
// re-driven through the factories must not crash.
BOOST_AUTO_TEST_CASE(RecordedCaptureFramesDoNotCrash)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);

	const std::vector<std::string> jandy_fixtures{
		"fixtures/sample_session.cap",
		"fixtures/onetouch_chlorinator.cap",
		"fixtures/iaq_boot_sequence.cap",
		"fixtures/iaq_onetouch_startup.cap",
		"fixtures/onetouch_setpoint_edit.cap",
	};

	std::size_t total_frames = 0;
	for (const auto& fixture : jandy_fixtures)
	{
		const auto frames = Test::LoadFixtureFrames(fixture);
		for (const auto& frame : frames)
		{
			DriveJandyGenerator(frame);
			DrivePentairGenerator(frame);
			++total_frames;
		}
	}

	// Pentair capture: drive the flat R-stream through the Pentair generator.
	{
		const auto stream = Test::LoadFixture("fixtures/alwin32/intelliflo.cap");
		DrivePentairGenerator(stream);
	}

	BOOST_TEST(total_frames > 0U);
}

BOOST_AUTO_TEST_SUITE_END()
