#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AqualinkAutomate::Test
{

	class MockReplayHarness;

	//=========================================================================
	// LoadFixture / ReplayFixture
	//
	// Reusable helpers for downstream end-to-end tests that want to drive a
	// CAPTURED RS-485 session (a recording in the RecordingSerialPortImpl format)
	// through the decode stack as a test fixture.
	//
	// A capture file is the developer recorder's on-disk format:
	//
	//   # comment / header / metadata lines (ignored)
	//   [<timestamp_ms>] R 0x10|0x02|...   <- bytes read FROM the device
	//   [<timestamp_ms>] W 0x10|0x02|...   <- bytes the app wrote TO the device
	//
	// Only the R-direction bytes form the incoming stream and are returned by
	// LoadFixture; W-lines, '#' comments and blanks are skipped.  The legacy bare
	// "0x10|0x02|..." token format is also accepted for back-compat.
	//
	// IMPORTANT: parsing is delegated to the production MockSerialPortImpl file
	// reader, so LoadFixture is byte-for-byte symmetric with what `--dev-mode
	// --replay-filename <file>` feeds the application at runtime.  Downstream
	// tests can therefore trust that what LoadFixture returns is exactly what a
	// real replay would deliver.
	//
	// Fixture-file resolution: a relative path is resolved against the test
	// working directory (CMake copies test/fixtures -> <build>/test/fixtures as a
	// POST_BUILD step), so `LoadFixture("fixtures/sample_session.cap")` works from
	// any test.  Absolute paths are used as-is.
	//=========================================================================

	// Load the replayable (R-direction / legacy) bytes of a capture file into a
	// flat byte vector.  Throws std::runtime_error if the file cannot be opened.
	std::vector<uint8_t> LoadFixture(const std::string& fixture_path);

	// As LoadFixture(), but split the flat stream into individual wire frames
	// (each a DLE/STX .. DLE/ETX packet, DLE-stuffing respected). Bytes outside a
	// frame are dropped. Use with Replay(frames) so each frame arrives in its own
	// read/Poll -- a single flat Replay() only drains one read chunk per Poll, so a
	// capture longer than one chunk would otherwise be truncated mid-stream.
	std::vector<std::vector<uint8_t>> LoadFixtureFrames(const std::string& fixture_path);

	// Convenience: LoadFixtureFrames() then Replay() each frame through the harness
	// (one read/Poll per frame). Returns the frames replayed.
	std::vector<std::vector<uint8_t>> ReplayFixtureFramed(MockReplayHarness& harness, const std::string& fixture_path);

	// Convenience: LoadFixture() the given capture and Replay() the resulting
	// byte stream through an already-constructed MockReplayHarness (devices the
	// caller wants to assert on must already be AddDevice()'d).  Returns the bytes
	// that were replayed (useful for sanity assertions).
	std::vector<uint8_t> ReplayFixture(MockReplayHarness& harness, const std::string& fixture_path);

}
// namespace AqualinkAutomate::Test
