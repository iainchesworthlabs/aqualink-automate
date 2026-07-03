#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/any_io_executor.hpp>

#include "utility/offload_pool.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// PasswordHasher — argon2id via libsodium (docs/auth-redesign.md §6, D4).
	//
	// Hash() emits the self-describing crypto_pwhash_str encoding (algorithm,
	// parameters and salt embedded), which is what UserRecord::PasswordHash
	// stores; Verify() checks a password against it.
	//
	// Hashing is DELIBERATELY slow (~100ms at the default interactive cost).
	// The application's entire runtime shares one cooperative thread with the
	// RS-485 protocol loop, so production callers MUST use the *Async forms,
	// which run the work on an OffloadPool and post the completion back to
	// the caller's executor.  The synchronous forms exist for the offload
	// lambdas and for tests (which pass TestParams() to keep suites fast).
	//=========================================================================
	class PasswordHasher
	{
	public:
		struct Params
		{
			std::uint64_t OpsLimit{ 0 };   // 0 == libsodium INTERACTIVE default.
			std::size_t MemLimit{ 0 };     // 0 == libsodium INTERACTIVE default.
		};

		// Minimum-cost parameters for unit tests ONLY (fast, insecure).
		static Params TestParams();

	public:
		// Throws std::runtime_error when sodium fails (init/allocation).
		//
		// No default for `params`: every call site passes one explicitly
		// (Params{} as a default argument here is a nested-class default-member-
		// initializer used before PasswordHasher is a complete class — MSVC
		// accepts it, GCC/Clang correctly reject it).
		static std::string Hash(std::string_view password, const Params& params);

		// Constant-time verification against a stored crypto_pwhash_str value.
		static bool Verify(std::string_view password, std::string_view stored_hash);

	public:
		template<typename Completion>  // void(std::string hash)
		static void HashAsync(Utility::OffloadPool& pool, boost::asio::any_io_executor executor, std::string password, Params params, Completion&& completion)
		{
			pool.Run(std::move(executor),
				[password = std::move(password), params]() { return Hash(password, params); },
				std::forward<Completion>(completion));
		}

		template<typename Completion>  // void(bool verified)
		static void VerifyAsync(Utility::OffloadPool& pool, boost::asio::any_io_executor executor, std::string password, std::string stored_hash, Completion&& completion)
		{
			pool.Run(std::move(executor),
				[password = std::move(password), stored_hash = std::move(stored_hash)]() { return Verify(password, stored_hash); },
				std::forward<Completion>(completion));
		}
	};

}
// namespace AqualinkAutomate::Auth
