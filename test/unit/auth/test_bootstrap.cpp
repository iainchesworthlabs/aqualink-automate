#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>

#include "auth/audit_log.h"
#include "auth/bootstrap.h"
#include "auth/group.h"
#include "auth/password_hasher.h"
#include "auth/user_store.h"

using namespace AqualinkAutomate;

namespace
{
	namespace fs = std::filesystem;

	struct BootstrapFixture
	{
		static fs::path MakeDir()
		{
			static std::uint32_t counter{ 0 };
			const auto dir = fs::temp_directory_path() / std::format("aa-bootstrap-test-{}", counter++);
			fs::create_directories(dir);
			return dir;
		}

		BootstrapFixture() :
			Dir(MakeDir()),
			Users(Auth::UserStore::Load(Dir / "users.json"))
		{
		}

		~BootstrapFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
		Auth::UserStore Users;
		Auth::AuditLog Audit{ {} };
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_Bootstrap)

BOOST_AUTO_TEST_CASE(Test_Bootstrap_PasswordPolicy)
{
	BOOST_CHECK(Auth::ValidatePasswordPolicy("short").has_value());
	BOOST_CHECK(Auth::ValidatePasswordPolicy("elevenchars").has_value());          // 11: too short.
	BOOST_CHECK(!Auth::ValidatePasswordPolicy("twelve-chars").has_value());        // 12: minimum.
	BOOST_CHECK(!Auth::ValidatePasswordPolicy("a much longer passphrase").has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_Bootstrap_CreatesFirstAdminOnceOnly, BootstrapFixture)
{
	const auto registry = Auth::GroupRegistry::WithBuiltIns();
	std::string error;

	// First call creates an Administrators-group admin...
	const auto id = Auth::BootstrapAdmin(Users, "admin", "a-long-enough-password", Auth::PasswordHasher::TestParams(), Audit, error);
	BOOST_REQUIRE_MESSAGE(id.has_value(), error);

	const auto admin = Users.FindByUsername("admin");
	BOOST_REQUIRE(admin.has_value());
	BOOST_CHECK(Users.HasEnabledAdmin(registry));
	BOOST_CHECK(Auth::PasswordHasher::Verify("a-long-enough-password", admin->PasswordHash));

	// ...and the system now has an owner: EVERY further bootstrap refuses.
	BOOST_CHECK(!Auth::BootstrapAdmin(Users, "second", "another-long-password", Auth::PasswordHasher::TestParams(), Audit, error).has_value());
	BOOST_CHECK(!Auth::BootstrapAdminWithHash(Users, "third", "$argon2id$fake", Audit, error).has_value());
	BOOST_CHECK_EQUAL(Users.Size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(Test_Bootstrap_RejectsWeakPasswordAndEmptyUsername, BootstrapFixture)
{
	std::string error;

	BOOST_CHECK(!Auth::BootstrapAdmin(Users, "admin", "weak", Auth::PasswordHasher::TestParams(), Audit, error).has_value());
	BOOST_CHECK(!Auth::BootstrapAdmin(Users, "", "a-long-enough-password", Auth::PasswordHasher::TestParams(), Audit, error).has_value());
	BOOST_CHECK(Users.Empty());
}

BOOST_AUTO_TEST_SUITE_END()
