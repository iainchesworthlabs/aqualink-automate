#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <boost/circular_buffer.hpp>
#include <boost/test/unit_test.hpp>

#include "jandy/generator/jandy_message_generator.h"
#include "jandy/generator/jandy_message_generator_packetvalidation.h"
#include "jandy/messages/jandy_message_ack.h"
#include "jandy/messages/jandy_message_constants.h"
#include "jandy/messages/jandy_message_message_long.h"
#include "jandy/messages/jandy_message_status.h"
#include "jandy/types/jandy_types.h"
#include "jandy/utility/jandy_null_handler.h"

#include "support/unit_test_circularbuffermaker.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Messages;
using namespace AqualinkAutomate::Utility;

//=============================================================================
// DLE byte-stuffing round-trip.
//
// On the Jandy RS-485 wire a literal payload byte of 0x10 (DLE) is escaped as
// the pair 0x10 0x00 so it cannot be mistaken for a frame marker; the receiver
// removes the inserted 0x00.  These tests prove the stuff/de-stuff is symmetric
// in BOTH directions:
//
//   1. The low-level utility pair (JandyPacket_NullCharHandler_Serialization /
//      _Deserialization) is its own inverse.
//   2. The full encode path (JandyMessage::Serialize) escapes a 0x10 payload
//      byte to 0x10 0x00 on the wire...
//   3. ...and the full decode path (the message generator) de-stuffs it back to
//      the original payload, validating framing + checksum over the unescaped
//      bytes.
//=============================================================================

namespace
{
	// Count occurrences of the escape pair 0x10 0x00 in a byte buffer.
	std::size_t CountDleNullPairs(const std::vector<uint8_t>& bytes)
	{
		std::size_t count = 0;
		for (std::size_t i = 0; i + 1 < bytes.size(); ++i)
		{
			if (bytes[i] == 0x10 && bytes[i + 1] == 0x00)
			{
				++count;
			}
		}
		return count;
	}
}

BOOST_AUTO_TEST_SUITE(JandyDleStuffing_TestSuite)

// ---------------------------------------------------------------------------
// 1. Low-level utility: serialize-escape then deserialize-unescape is identity
//    for a payload containing a literal 0x10.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(NullHandler_StuffThenDeStuff_RoundTripsPayloadDle)
{
	// A minimally-framed raw packet whose payload (index 4) is a literal 0x10:
	// [DLE STX dest cmd 0x10 checksum DLE ETX].  The escaper only touches the
	// region between index 2 and the trailing checksum/DLE/ETX, matching the
	// production Serialize() contract.
	std::vector<uint8_t> raw = { 0x10, 0x02, 0x42, 0x03, 0x10, 0x00, 0x10, 0x03 };
	const std::vector<uint8_t> original = raw;

	// Forward (generate): the literal 0x10 payload byte gains a trailing 0x00.
	std::vector<uint8_t> wire = raw;
	JandyPacket_NullCharHandler_Serialization(wire);
	BOOST_TEST(wire.size() > original.size());
	BOOST_TEST(CountDleNullPairs(wire) >= 1u);

	// Reverse (parse): de-stuffing the wire bytes restores the exact original.
	std::vector<uint8_t> decoded = wire;
	JandyPacket_NullCharHandler_Deserialization(decoded);
	BOOST_TEST(decoded == original, boost::test_tools::per_element());
}

// ---------------------------------------------------------------------------
// 1b. The span-based de-stuffer agrees with the in-place de-stuffer and the
//     NeedsNullCharHandling predicate detects the escape.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(NullHandler_SpanDeStuff_MatchesInPlace)
{
	std::vector<uint8_t> raw = { 0x10, 0x02, 0x42, 0x03, 0x10, 0x00, 0x10, 0x03 };

	std::vector<uint8_t> wire = raw;
	JandyPacket_NullCharHandler_Serialization(wire);

	BOOST_TEST(JandyPacket_NeedsNullCharHandling(std::span<const uint8_t>(wire)));

	std::vector<uint8_t> out(wire.size(), 0x00);
	const std::size_t written = JandyPacket_NullCharHandler_DeserializationToSpan(
		std::span<const uint8_t>(wire), std::span<uint8_t>(out));
	out.resize(written);

	BOOST_TEST(out == raw, boost::test_tools::per_element());
}

// ---------------------------------------------------------------------------
// 1c. Regression: when the output span is SMALLER than the de-stuffed input,
//     the span de-stuffer must never write past output.size().  Previously the
//     write was unbounded (latent OOB write).  Here the input has no escape
//     pairs to remove, so every byte is a candidate write — the function must
//     stop once the output is full and report the clamped count.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(NullHandler_SpanDeStuff_ClampsWhenOutputTooSmall)
{
	// 16 distinct, escape-free bytes (no 0x10 0x00 pair) so de-stuffing removes
	// nothing and the writer would otherwise emit all 16 bytes.
	std::vector<uint8_t> input;
	for (uint8_t value = 0; value < 16; ++value)
	{
		input.push_back(static_cast<uint8_t>(0x20 + value));
	}

	// A guarded output buffer: a 4-byte writable window flanked by sentinel
	// bytes.  If the function overruns, a sentinel changes and the test fails.
	constexpr uint8_t sentinel = 0xEE;
	constexpr std::size_t window = 4;
	std::vector<uint8_t> guarded(window + 4, sentinel);
	std::span<uint8_t> output(guarded.data() + 2, window);

	const std::size_t written = JandyPacket_NullCharHandler_DeserializationToSpan(
		std::span<const uint8_t>(input), output);

	// Exactly the window's worth of bytes were written and the count is clamped.
	BOOST_CHECK_EQUAL(written, window);

	// The written window matches the first `window` input bytes...
	for (std::size_t i = 0; i < window; ++i)
	{
		BOOST_CHECK_EQUAL(output[i], input[i]);
	}

	// ...and the guard sentinels on BOTH sides are untouched (no OOB write).
	BOOST_CHECK_EQUAL(guarded.front(), sentinel);
	BOOST_CHECK_EQUAL(guarded[1], sentinel);
	BOOST_CHECK_EQUAL(guarded[guarded.size() - 2], sentinel);
	BOOST_CHECK_EQUAL(guarded.back(), sentinel);
}

// ---------------------------------------------------------------------------
// 1d. Regression: an output span that is exactly one byte too small to hold the
//     de-stuffed result (escape pair present) must clamp to output.size() and
//     not overflow.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(NullHandler_SpanDeStuff_ClampsAtBoundaryWithEscape)
{
	// Wire form contains one 0x10 0x00 escape pair, so the de-stuffed result is
	// one byte shorter than the input.
	std::vector<uint8_t> raw = { 0x10, 0x02, 0x42, 0x03, 0x10, 0x00, 0x10, 0x03 };
	std::vector<uint8_t> wire = raw;
	JandyPacket_NullCharHandler_Serialization(wire);

	BOOST_REQUIRE(JandyPacket_NeedsNullCharHandling(std::span<const uint8_t>(wire)));

	// Size the output one byte SHORTER than the correct de-stuffed length.
	BOOST_REQUIRE(raw.size() >= 1u);
	const std::size_t too_small = raw.size() - 1u;

	std::vector<uint8_t> out(too_small, 0x00);
	const std::size_t written = JandyPacket_NullCharHandler_DeserializationToSpan(
		std::span<const uint8_t>(wire), std::span<uint8_t>(out));

	// The function never reports more bytes than the output could hold.
	BOOST_CHECK_EQUAL(written, too_small);
	BOOST_CHECK_LE(written, out.size());
}

// ---------------------------------------------------------------------------
// 2 + 3. Full stack: Serialize() a message whose payload contains 0x10, prove
//        the escape appears on the wire, then parse it back through the message
//        generator and prove the decoded payload matches the original.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(MessageGenerator_SerializeThenParse_RoundTripsPayloadDle)
{
	// An ACK whose Command() byte is 0x10 forces a literal DLE into the payload
	// (AckType at index 4, Command at index 5).
	constexpr uint8_t ack_type_value = 0x00;
	constexpr uint8_t command_with_dle = 0x10;

	JandyMessage_Ack tx(static_cast<AckTypes>(ack_type_value), command_with_dle);

	std::vector<uint8_t> wire;
	BOOST_REQUIRE(tx.Serialize(wire));

	// The encoder must have escaped the literal 0x10 payload byte to 0x10 0x00.
	BOOST_TEST(CountDleNullPairs(wire) >= 1u);

	// Feed the escaped wire bytes through the real message generator (framing +
	// checksum validation happen over the de-stuffed bytes).
	boost::circular_buffer<uint8_t> buffer(wire.size());
	buffer.assign(wire.begin(), wire.end());

	auto result = Generators::GenerateMessageFromRawData(buffer);
	BOOST_REQUIRE(result.has_value());

	auto base_msg = result.value();
	BOOST_REQUIRE(nullptr != base_msg);

	// The generator created the right concrete type and the de-stuffed payload
	// decoded back to the original command byte (0x10) — proving the reverse
	// direction undoes the forward escape end-to-end.
	auto* ack = dynamic_cast<JandyMessage_Ack*>(base_msg.get());
	BOOST_REQUIRE(nullptr != ack);
	BOOST_CHECK_EQUAL(static_cast<uint8_t>(ack->AckType()), ack_type_value);
	BOOST_CHECK_EQUAL(ack->Command(), command_with_dle);
}

// ---------------------------------------------------------------------------
// 4. Regression: a frame whose DESTINATION byte is 0x10 (== DLE) is stuffed on
//    the wire as {0x10, 0x00}, which pushes the message-type byte one position
//    later. The factory selects the type from the still-stuffed bytes, so it must
//    account for this -- otherwise it reads the inserted 0x00 and mis-types the
//    frame as a Probe. This is the exact on-wire frame the master sends a spa-side
//    Dual Spa Switch at 0x10 (a cmd-0x02 LED image); it must decode as a Status
//    addressed to 0x10, not a Probe. (Bug: any device at 0x10 was never recognised.)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(MessageGenerator_DleValuedDestination_TypedByRealCommandNotStuffByte)
{
	const std::vector<uint8_t> wire =
	{
		0x10, 0x02,             // DLE STX (frame start)
		0x10, 0x00,             // destination 0x10, DLE-stuffed with 0x00
		0x02,                   // command 0x02 (Status / the LED-image poll)
		0x00, 0x00, 0x00, 0x00, 0x00,  // payload
		0x24,                   // checksum = (0x10+0x02+0x10+0x02) & 0xFF
		0x10, 0x03              // DLE ETX (frame end)
	};

	boost::circular_buffer<uint8_t> buffer(wire.size());
	buffer.assign(wire.begin(), wire.end());

	auto result = Generators::GenerateMessageFromRawData(buffer);
	BOOST_REQUIRE(result.has_value());

	auto base_msg = result.value();
	BOOST_REQUIRE(nullptr != base_msg);

	// Must be a Status (cmd 0x02) addressed to 0x10 -- NOT a Probe (the 0x00 stuff byte).
	auto* status = dynamic_cast<JandyMessage_Status*>(base_msg.get());
	BOOST_REQUIRE_MESSAGE(nullptr != status, "frame to DLE-valued destination 0x10 was mis-typed (expected Status)");
	BOOST_CHECK_EQUAL(static_cast<unsigned>(status->Destination().Id()()), 0x10u);
}

// ---------------------------------------------------------------------------
// 5. Regression: the CHECKSUM byte itself can equal 0x10 (DLE). Real Jandy
//    hardware DLE-stuffs it exactly like any other content byte -- {0x10, 0x00}
//    takes the place of the single checksum byte, one position earlier than the
//    usual 3-byte footer. PacketValidation_ChecksumIsValid() runs on the raw,
//    still-stuffed wire bytes (de-stuffing happens later); previously it always
//    assumed the checksum sat exactly 3 bytes from the end, so a stuffed checksum
//    byte was misread as the stuffing pad (0x00) and the true checksum byte got
//    folded into the summed region a second time -- always doubling the
//    calculated value and rejecting an otherwise perfectly valid frame. This is
//    the exact bug that silently dropped real OneTouch clock/display updates
//    (MessageLong / 0x04, LineId 3) captured from the user's own panel in
//    test/fixtures/iaq_onetouch_startup.cap and test/fixtures/onetouch_chlorinator.cap.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(PacketValidation_ChecksumByteEqualsDle_IsStuffedAndValidates)
{
	// dest=0x00, type=Ack(0x01), AckType=0x00, Command=0xFD:
	// checksum = (0x10+0x02+0x00+0x01+0x00+0xFD) & 0xFF = 0x110 & 0xFF = 0x10.
	// The checksum byte, being 0x10, is DLE-stuffed on the wire: {0x10, 0x00}
	// stands in for the single checksum byte, immediately followed by the
	// unescaped DLE/ETX footer.
	auto buffer = Test::MakeCircularBuffer<uint8_t>({ 0x10, 0x02, 0x00, 0x01, 0x00, 0xFD, 0x10, 0x00, 0x10, 0x03 });

	BOOST_TEST(Generators::PacketValidation_ChecksumIsValid(buffer.begin(), buffer.end()));
}

BOOST_AUTO_TEST_CASE(PacketValidation_NonStuffedChecksum_StillValidates)
{
	// Same shape, but Command chosen so the checksum is NOT 0x10 -- the ordinary,
	// non-stuffed 3-byte footer must still validate exactly as before.
	// checksum = (0x10+0x02+0x00+0x01+0x00+0x02) & 0xFF = 0x15.
	auto buffer = Test::MakeCircularBuffer<uint8_t>({ 0x10, 0x02, 0x00, 0x01, 0x00, 0x02, 0x15, 0x10, 0x03 });

	BOOST_TEST(Generators::PacketValidation_ChecksumIsValid(buffer.begin(), buffer.end()));
}

BOOST_AUTO_TEST_CASE(PacketValidation_GenuinelyCorruptedChecksum_StillFails)
{
	// A deliberately wrong checksum (0x00 instead of the correct 0x15) that does
	// NOT take the DLE-stuffed shape must still be rejected.
	auto buffer = Test::MakeCircularBuffer<uint8_t>({ 0x10, 0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x10, 0x03 });

	BOOST_TEST(!Generators::PacketValidation_ChecksumIsValid(buffer.begin(), buffer.end()));
}

BOOST_AUTO_TEST_CASE(MessageGenerator_DleValuedChecksum_SerializeThenParse_RoundTrips)
{
	// Same Ack shape as above (checksum lands on 0x10), but built and parsed through
	// the real production Serialize()/GenerateMessageFromRawData() stack -- proving
	// the encoder now escapes ITS OWN checksum byte (previously excluded from the
	// scan) and that the generator's fixed checksum gatekeeper accepts the result.
	constexpr uint8_t ack_type_value = 0x00;
	constexpr uint8_t command_value = 0xFD;

	JandyMessage_Ack tx(static_cast<AckTypes>(ack_type_value), command_value);

	std::vector<uint8_t> wire;
	BOOST_REQUIRE(tx.Serialize(wire));

	// Neither AckType (0x00) nor Command (0xFD) is 0x10, so the only possible source
	// of an escape pair on the wire is the checksum byte itself.
	BOOST_TEST(CountDleNullPairs(wire) >= 1u);

	boost::circular_buffer<uint8_t> buffer(wire.size());
	buffer.assign(wire.begin(), wire.end());

	auto result = Generators::GenerateMessageFromRawData(buffer);
	BOOST_REQUIRE(result.has_value());

	auto base_msg = result.value();
	BOOST_REQUIRE(nullptr != base_msg);

	auto* ack = dynamic_cast<JandyMessage_Ack*>(base_msg.get());
	BOOST_REQUIRE(nullptr != ack);
	BOOST_CHECK_EQUAL(static_cast<uint8_t>(ack->AckType()), ack_type_value);
	BOOST_CHECK_EQUAL(ack->Command(), command_value);
}

BOOST_AUTO_TEST_CASE(MessageGenerator_RealCapture_OneTouchClockLine_ChecksumByteStuffed)
{
	// Captured verbatim from the user's real AquaLink panel
	// (test/fixtures/iaq_onetouch_startup.cap, lines 1131-1132): a MessageLong (0x04)
	// "set display line" addressed to the OneTouch (dest 0x41), LineId 3, text
	// "    11:39 AM    ". Its checksum computes to 0x10 and is DLE-stuffed on the
	// wire; before this fix the frame was rejected as a checksum failure and
	// silently dropped, so the OneTouch clock display never updated.
	auto buffer = Test::MakeCircularBuffer<uint8_t>({
		0x10, 0x02, 0x41, 0x04, 0x03,
		0x20, 0x20, 0x20, 0x20, 0x31, 0x31, 0x3a, 0x33, 0x39, 0x20, 0x41, 0x4d, 0x20, 0x20, 0x20, 0x20,
		0x10, 0x00, 0x10, 0x03
	});

	auto result = Generators::GenerateMessageFromRawData(buffer);
	BOOST_REQUIRE(result.has_value());

	auto base_msg = result.value();
	BOOST_REQUIRE(nullptr != base_msg);

	auto* line = dynamic_cast<JandyMessage_MessageLong*>(base_msg.get());
	BOOST_REQUIRE_MESSAGE(nullptr != line, "OneTouch clock-line frame with a DLE-stuffed checksum byte was rejected (expected MessageLong)");
	BOOST_CHECK_EQUAL(static_cast<unsigned>(line->LineId()), 3u);
	BOOST_CHECK_EQUAL(line->Line(), "    11:39 AM    ");
}

BOOST_AUTO_TEST_SUITE_END()
