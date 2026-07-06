//
// libFuzzer harness: JWT verification robustness.
//
// Auth::JwtCodec::Verify decodes and verifies an untrusted bearer token
// (Authorization header / WebSocket subprotocol). Its documented contract is
// "returns std::nullopt on any failure" — it must never throw or crash on a
// malformed token. This harness feeds arbitrary bytes as the token through a codec
// backed by an EMPTY JwtKeyStore, so signature verification always fails; that
// exercises the jwt-cpp decode + the first-party `kid` extraction and guards the
// no-throw contract. (The claim-extraction path past signature check is only
// reachable with a validly-signed token, which a mutation fuzzer cannot forge — see
// docs/fuzzing.md.) Build: fuzz/CMakeLists.txt.
//

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "logging/logging_severity_filter.h"

using namespace AqualinkAutomate;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
	Logging::SeverityFiltering::SetGlobalFilterLevel(Logging::Severity::Fatal);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// Built once per process: a throwaway key store (JwtKeyStore's default ctor is
	// private; LoadOrCreate is the factory). The fuzzed token can never be validly
	// signed by it, so signature verification always fails — which is what we want:
	// this fuzzes the jwt-cpp decode + first-party kid extraction and the no-throw
	// contract, not the (unforgeable) post-verification claim path.
	static const auto key_store = std::make_shared<Auth::JwtKeyStore>(
		Auth::JwtKeyStore::LoadOrCreate(std::filesystem::temp_directory_path() / "aa_fuzz_jwt_keys.json"));
	static const Auth::JwtCodec codec(key_store, Auth::JwtCodec::Config{});

	std::string error;
	(void)codec.Verify(std::string(reinterpret_cast<const char*>(data), size), &error);

	return 0;
}
