#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// AuditLog — the security audit trail (docs/auth-redesign.md §10, D17).
	//
	// Every auditable event (control action with its PDP decision, login
	// success/failure, lockout, token/key issuance + revocation, entitlement/
	// group change, posture change) is recorded twice:
	//
	//   1. as a structured JSONL line in the state-dir audit file (owner-only;
	//      size-rotated) — feeds the in-app audit viewer and is the fallback
	//      sink that always exists; and
	//   2. through the Logging facade on Channel::Audit — which the OS-native
	//      sink (syslog/journald on POSIX, Windows Event Log on Windows; see
	//      RegisterAuditOsSink) forwards to the platform's audit trail.
	//=========================================================================

	struct AuditEvent
	{
		std::string SubjectId{ "anonymous" };
		std::string Provider{};        // "Local", "ApiKey", "Anonymous", ...
		std::string Action{};          // Entitlement action, or auth event name ("auth.login", ...).
		std::string ResourceKind{};    // Route-declared resource category ("aux", ...).
		std::string ResourceId{};      // Specific instance ("AUX3", user id, key id, ...).
		std::string Decision{};        // "permit" | "deny" | "success" | "failure".
		std::string PeerIp{};          // Trusted-proxy-aware client address.
		std::string Detail{};          // Free-form context (never secrets).
	};

	class AuditLog
	{
	public:
		struct Config
		{
			// Empty path disables the JSONL sink (channel emission remains).
			std::filesystem::path JsonlFile{};

			// Size-based rotation: when the file would exceed this, it is
			// renamed to "<name>.1" (replacing any previous) and restarted.
			std::uintmax_t MaxFileBytes{ 10ull * 1024ull * 1024ull };
		};

	public:
		explicit AuditLog(Config config);

	public:
		void Record(const AuditEvent& event);

	private:
		void AppendJsonl(const AuditEvent& event);
		void RotateIfNeeded(std::uintmax_t incoming_bytes);

	private:
		Config m_Config;
	};

	// Register the platform's OS-native audit sink on the logging core,
	// filtered to Channel::Audit: syslog on POSIX, the Windows Event Log on
	// Windows.  Returns false (after logging a warning) when the platform
	// sink cannot be installed — the JSONL file remains the durable trail.
	bool RegisterAuditOsSink();

}
// namespace AqualinkAutomate::Auth
