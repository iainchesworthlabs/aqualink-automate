#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <boost/log/sinks/sink.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// AuditLog — the security audit trail (docs/auth-redesign.md §10, D17;
	// docs/logging-sinks-redesign.md §10).
	//
	// Audit is a subsystem, NOT an operational log channel. Every auditable event
	// (control action with its PDP decision, login success/failure, lockout,
	// token/key issuance + revocation, entitlement/group change, posture change)
	// reaches:
	//
	//   1. the OS-native, forward-capable sink — syslog LOG_AUTHPRIV on POSIX or a
	//      dedicated Windows Event Log source (see RegisterAuditOsSink) — the
	//      AUTHORITATIVE copy (integrity comes from off-box forwarding); and
	//   2. a structured JSONL line in the state-dir audit file (owner-only,
	//      size-rotated) — convenience/fallback that feeds the in-app audit viewer;
	//      explicitly NOT the integrity anchor.
	//
	// Emission uses the Boost.Log core tagged with the `is_audit` attribute (not a
	// Logging::Channel), so audit records reach only the audit sink and never an
	// operational sink.
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
			std::uintmax_t MaxFileBytes{ 10ULL * 1024ULL * 1024ULL };
		};

	public:
		explicit AuditLog(Config config);

		void Record(const AuditEvent& event);

	private:
		void AppendJsonl(const AuditEvent& event);
		void RotateIfNeeded(std::uintmax_t incoming_bytes) const;

	private:
		Config m_Config;
	};

	// Register the audit trail's OS-native sink on the logging core, accepting only
	// audit records (the `is_audit` attribute): syslog with the LOG_AUTHPRIV facility
	// on POSIX, a dedicated Windows Event Log source on Windows. Returns the installed
	// sink as a removable handle, or an empty (null) shared_ptr — after logging a
	// warning — when the platform sink cannot be installed (the JSONL file remains
	// the local fallback).
	//
	// Production callers install for the process lifetime and may ignore the
	// handle; callers that need a bounded lifetime (e.g. tests) keep the handle and
	// pass it to boost::log::core::get()->remove_sink(handle) in teardown.
	boost::shared_ptr<boost::log::sinks::sink> RegisterAuditOsSink();

}
// namespace AqualinkAutomate::Auth
