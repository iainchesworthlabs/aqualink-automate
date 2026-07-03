#include <cstddef>
#include <format>
#include <utility>

#include "auth/bootstrap.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr std::size_t MIN_PASSWORD_LENGTH{ 12 };
	}
	// anonymous namespace

	std::optional<std::string> ValidatePasswordPolicy(std::string_view password)
	{
		if (password.size() < MIN_PASSWORD_LENGTH)
		{
			return std::format("Password must be at least {} characters", MIN_PASSWORD_LENGTH);
		}

		return std::nullopt;
	}

	std::optional<std::string> BootstrapAdminWithHash(UserStore& users, std::string_view username, std::string password_hash, AuditLog& audit, std::string& error)
	{
		if (!users.Empty())
		{
			// The system already has an owner: bootstrap is over.  A replayed
			// setup call or stale --bootstrap-admin flag must never mint an
			// extra administrator.
			error = "Setup has already been completed";
			return std::nullopt;
		}

		if (username.empty())
		{
			error = "Username is required";
			return std::nullopt;
		}

		UserRecord admin;
		admin.Username = std::string{ username };
		admin.PasswordHash = std::move(password_hash);
		admin.Groups = { std::string{ BuiltInGroups::ADMINISTRATORS } };

		if (!users.Create(std::move(admin), error))
		{
			return std::nullopt;
		}

		const auto created = users.FindByUsername(username);

		AuditEvent event;
		event.SubjectId = created->Id;
		event.Provider = "Local";
		event.Action = "auth.bootstrap_admin";
		event.Decision = "success";
		event.Detail = "first administrator created";
		audit.Record(event);

		return created->Id;
	}

	std::optional<std::string> BootstrapAdmin(UserStore& users, std::string_view username, std::string_view password, const PasswordHasher::Params& hash_params, AuditLog& audit, std::string& error)
	{
		if (const auto policy_error = ValidatePasswordPolicy(password); policy_error.has_value())
		{
			error = *policy_error;
			return std::nullopt;
		}

		return BootstrapAdminWithHash(users, username, PasswordHasher::Hash(password, hash_params), audit, error);
	}

}
// namespace AqualinkAutomate::Auth
