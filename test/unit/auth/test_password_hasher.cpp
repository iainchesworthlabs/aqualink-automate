#include <boost/test/unit_test.hpp>

#include <string>
#include <thread>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "auth/password_hasher.h"
#include "utility/offload_pool.h"

using namespace AqualinkAutomate;

BOOST_AUTO_TEST_SUITE(TestSuite_PasswordHasher)

BOOST_AUTO_TEST_CASE(Test_PasswordHasher_HashAndVerifyRoundTrip)
{
	const auto hash = Auth::PasswordHasher::Hash("correct horse battery staple", Auth::PasswordHasher::TestParams());

	// Self-describing argon2id encoding; never the plaintext.
	BOOST_CHECK(hash.starts_with("$argon2id$"));
	BOOST_CHECK(hash.find("correct horse") == std::string::npos);

	BOOST_CHECK(Auth::PasswordHasher::Verify("correct horse battery staple", hash));
	BOOST_CHECK(!Auth::PasswordHasher::Verify("incorrect horse", hash));
	BOOST_CHECK(!Auth::PasswordHasher::Verify("", hash));
}

BOOST_AUTO_TEST_CASE(Test_PasswordHasher_SaltedHashesDiffer)
{
	const auto params = Auth::PasswordHasher::TestParams();

	// Same password, fresh salt every time.
	BOOST_CHECK(Auth::PasswordHasher::Hash("password-12345", params) != Auth::PasswordHasher::Hash("password-12345", params));
}

BOOST_AUTO_TEST_CASE(Test_PasswordHasher_VerifyRejectsGarbageHash)
{
	BOOST_CHECK(!Auth::PasswordHasher::Verify("anything", "not-an-argon2-encoding"));
	BOOST_CHECK(!Auth::PasswordHasher::Verify("anything", ""));
}

BOOST_AUTO_TEST_CASE(Test_PasswordHasher_AsyncRunsOffCallerThread)
{
	boost::asio::io_context io_context;
	auto guard = boost::asio::make_work_guard(io_context);

	Utility::OffloadPool pool{ 1 };

	const auto caller_thread = std::this_thread::get_id();
	std::thread::id completion_thread{};
	bool verified{ false };

	const auto hash = Auth::PasswordHasher::Hash("swordfish-swordfish", Auth::PasswordHasher::TestParams());

	Auth::PasswordHasher::VerifyAsync(pool, io_context.get_executor(), "swordfish-swordfish", hash,
		[&](bool result)
		{
			completion_thread = std::this_thread::get_id();
			verified = result;
			guard.reset();
		});

	io_context.run();

	BOOST_CHECK(verified);
	BOOST_CHECK(completion_thread == caller_thread);  // State touched on the kernel thread only.
}

BOOST_AUTO_TEST_SUITE_END()
