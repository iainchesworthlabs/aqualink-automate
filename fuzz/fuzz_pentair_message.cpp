//
// libFuzzer harness: Pentair RS-485 message decode path.
//
// Feeds arbitrary attacker-controlled bytes into the two highest-level Pentair
// "raw bytes -> message" entry points, exercising preamble scanning, 16-bit
// big-endian checksum validation, the command->type factory and every concrete
// Pentair message type's DeserializeContents:
//
//   1. RAW  -> GenerateMessageFromRawData(circular_buffer): preamble search,
//      length/frame-completeness handling and checksum rejection.
//
//   2. WRAPPED -> a valid, checksum-correct frame is built around a mutated
//      (from, dest, command, data) tuple and pushed through BOTH the factory
//      (CreateFromSerialData, which operates on the checksummed region) and the
//      generator (which operates on the full preambled frame).
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

#include "factories/pentair_message_factory.h"
#include "generator/pentair_message_generator.h"
#include "messages/pentair_message.h"
#include "logging/logging_severity_filter.h"

#include "fuzz_frame_builders.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

namespace
{

	void FeedGenerator(std::span<const uint8_t> bytes)
	{
		boost::circular_buffer<uint8_t> buffer(bytes.size() + 8U);
		buffer.insert(buffer.end(), bytes.begin(), bytes.end());
		buffer.linearize();

		(void)Pentair::Generators::GenerateMessageFromRawData(buffer);
	}

}
// unnamed namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::span<const uint8_t> input(data, size);

	// 1. Raw bytes straight into the protocol read engine.
	FeedGenerator(input);

	// 2. Wrap a mutated payload in a valid frame so the per-type deserialisers run.
	//    First three bytes steer (from, dest, command); the rest is the DATA section.
	if (size >= 3U)
	{
		const uint8_t from = data[0];
		const uint8_t dest = data[1];
		const uint8_t command = data[2];
		const std::span<const uint8_t> payload = input.subspan(3U);

		const std::vector<uint8_t> frame = Fuzzing::BuildPentairFrame(from, dest, command, payload);

		(void)Pentair::Factory::PentairMessageFactory::CreateFromSerialData(Fuzzing::PentairChecksummedRegion(frame));
		FeedGenerator(frame);
	}

	return 0;
}
