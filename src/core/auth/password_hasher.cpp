#include <stdexcept>

#include <sodium.h>

#include "auth/password_hasher.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		void EnsureSodiumInitialised()
		{
			// sodium_init() is idempotent (returns 1 when already initialised)
			// and thread-safe; only a negative return is a failure.
			static const int result = sodium_init();

			if (result < 0)
			{
				throw std::runtime_error("libsodium initialisation failed");
			}
		}
	}
	// anonymous namespace

	PasswordHasher::Params PasswordHasher::TestParams()
	{
		return Params{ .OpsLimit = crypto_pwhash_OPSLIMIT_MIN, .MemLimit = crypto_pwhash_MEMLIMIT_MIN };
	}

	std::string PasswordHasher::Hash(std::string_view password, const Params& params)
	{
		EnsureSodiumInitialised();

		const auto ops = (0 != params.OpsLimit) ? params.OpsLimit : crypto_pwhash_OPSLIMIT_INTERACTIVE;
		const auto mem = (0 != params.MemLimit) ? params.MemLimit : crypto_pwhash_MEMLIMIT_INTERACTIVE;

		char encoded[crypto_pwhash_STRBYTES];

		if (0 != crypto_pwhash_str(encoded, password.data(), password.size(), ops, mem))
		{
			throw std::runtime_error("argon2id password hashing failed (out of memory?)");
		}

		return std::string{ encoded };
	}

	bool PasswordHasher::Verify(std::string_view password, std::string_view stored_hash)
	{
		EnsureSodiumInitialised();

		// crypto_pwhash_str_verify needs the NUL-terminated encoded form.
		const std::string encoded{ stored_hash };

		return 0 == crypto_pwhash_str_verify(encoded.c_str(), password.data(), password.size());
	}

}
// namespace AqualinkAutomate::Auth
