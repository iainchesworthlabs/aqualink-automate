//
// libFuzzer harness: Jandy/Zodiac RS-485 message decode path.
//
// Feeds arbitrary attacker-controlled bytes into the two highest-level Jandy
// "raw bytes -> message" entry points, which between them exercise framing,
// DLE-null de-escaping, checksum validation, the compile-time message factory and
// every concrete message type's DeserializeContents:
//
//   1. RAW  -> GenerateMessageFromRawData(circular_buffer): the full protocol
//      read engine, including packet-boundary scanning, buffer cleanup, overlap
//      handling and checksum rejection.  Most random inputs are rejected here, but
//      the framing/scan/cleanup code itself is fuzzed on every input.
//
//   2. WRAPPED -> a valid, checksum-correct frame is built around a mutated
//      (destination, message-type, payload) triple and pushed through BOTH the
//      factory (CreateFromSerialData) and the generator, so a mutated payload
//      actually reaches the per-message-type deserialiser under test.
//
// Build: see fuzz/CMakeLists.txt (ENABLE_FUZZING).  Any crash / ASan report is a
// real bug in an untrusted-input parser — fix the production deserialiser and add
// a regression test; never weaken the parser to silence the fuzzer.
//

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <boost/circular_buffer.hpp>

#include "factories/jandy_message_factory.h"
#include "factories/jandy_message_factory_registration.h"
#include "generator/jandy_message_generator.h"
#include "messages/jandy_message.h"
#include "logging/logging_severity_filter.h"

#include "fuzz_frame_builders.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	// Silence the operational log: the decoders emit Trace/Debug records on every
	// malformed frame, which would dominate the fuzzer's run time and output.
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

namespace
{

	void FeedGenerator(std::span<const uint8_t> bytes)
	{
		// Size the ring generously: the generator scans and erases in place, and the
		// protocol task linearises before parsing, so give it room to hold the input.
		boost::circular_buffer<uint8_t> buffer(bytes.size() + 8U);
		buffer.insert(buffer.end(), bytes.begin(), bytes.end());
		buffer.linearize();

		// One pass is enough for a harness: exercise a single decode attempt.
		(void)Generators::GenerateMessageFromRawData(buffer);
	}

}
// unnamed namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::span<const uint8_t> input(data, size);

	// 1. Raw bytes straight into the protocol read engine.
	FeedGenerator(input);

	// 2. Wrap a mutated payload in a valid frame so the per-type deserialisers run.
	//    First two bytes steer (destination, message-type); the rest is payload.
	if (size >= 2U)
	{
		const uint8_t destination = data[0];
		const uint8_t message_type = data[1];
		const std::span<const uint8_t> payload = input.subspan(2U);

		const std::vector<uint8_t> frame = Fuzzing::BuildJandyFrame(destination, message_type, payload);

		(void)Factory::JandyMessageFactoryT::CreateFromSerialData(frame);
		FeedGenerator(frame);
	}

	return 0;
}
