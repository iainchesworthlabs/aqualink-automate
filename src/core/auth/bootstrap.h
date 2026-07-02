#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "auth/audit_log.h"
#include "auth/group.h"
#include "auth/password_hasher.h"
#include "auth/user_store.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// First-admin bootstrap (docs/auth-redesign.md §6, D4).
	//
	// Two paths create the FIRST administrator when --auth-mode is enabled
	// and the user store is empty:
	//
	//   - headless: --bootstrap-admin <username> with the password from
	//     --bootstrap-admin-password-file or the AQUALINK_BOOTSTRAP_ADMIN_
	//     PASSWORD environment variable (never the bare command line, which
	//     leaks via process listings), applied at startup; and
	//   - interactive: the first-run setup screen posting to /api/auth/setup.
	//
	// Both funnel through BootstrapAdmin(): idempotent — with ANY user on
	// file it refuses (the system has an owner; setup is over), so a stray
	// flag or replayed setup call can never mint an extra admin.
	//=========================================================================

	// The one password rule enforced everywhere a password is set (D19-adj):
	// at least 12 characters; no composition rules (length beats complexity).
	std::optional<std::string> ValidatePasswordPolicy(std::string_view password);

	// Create the first admin (Administrators group) from a PRE-COMPUTED
	// argon2id hash.  Returns the new user id, or nullopt with `error` set
	// (store not empty, empty username).  This is the serialisation point:
	// it runs on the kernel thread, so two racing setup attempts cannot both
	// pass the emptiness check.
	std::optional<std::string> BootstrapAdminWithHash(UserStore& users, std::string_view username, std::string password_hash, AuditLog& audit, std::string& error);

	// Convenience for STARTUP (--bootstrap-admin; the kernel loop is not yet
	// running, so the synchronous argon2 hash is acceptable here): policy
	// check + hash + BootstrapAdminWithHash.
	std::optional<std::string> BootstrapAdmin(UserStore& users, std::string_view username, std::string_view password, const PasswordHasher::Params& hash_params, AuditLog& audit, std::string& error);

}
// namespace AqualinkAutomate::Auth
