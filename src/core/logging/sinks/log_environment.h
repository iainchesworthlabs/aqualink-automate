#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace AqualinkAutomate::Logging::Sinks
{

	//
	// The operational sinks the `--log-sinks auto` policy resolves to for a given
	// environment (docs/logging-sinks-redesign.md §6.2). The audit trail is a
	// separate subsystem and is NOT represented here (§10).
	//
	struct SinkSelection
	{
		bool Console = false;
		bool ConsoleJournaldPrefixes = false;  // console emits sd-daemon "<N>" priority prefixes
		bool Native = false;                   // general OS-native sink (syslog / Event Log)
		bool File = false;                     // file sink (only when a log-file path is configured)
	};

	//
	// Device/inode identity of a file descriptor, used to test whether stderr is the
	// same stream systemd advertised in $JOURNAL_STREAM.
	//
	struct DevIno
	{
		std::uint64_t Device = 0;
		std::uint64_t Inode = 0;
	};

	//
	// The observed environment, captured as plain data so ResolveAutoSinks() is a
	// pure function of it and tests can construct any scenario without real syscalls.
	//
	struct LogEnvironment
	{
		bool StderrIsTty = false;
		bool StderrIsJournal = false;        // $JOURNAL_STREAM matches fstat(stderr) — POSIX/systemd only
		bool WindowsServiceContext = false;  // running as a Windows service (set by the service entry point)
	};

	//
	// Injectable raw probes. Production wires these to the real OS calls
	// (DefaultProbes); tests substitute fakes to force any environment. Each probe
	// is independent so a test can vary one axis at a time.
	//
	struct EnvironmentProbes
	{
		std::function<bool()> StderrIsTty;
		std::function<std::optional<std::string>(const char* name)> GetEnvVar;
		std::function<std::optional<DevIno>()> StatStderr;
		std::function<bool()> WindowsServiceContext;
	};

	[[nodiscard]] EnvironmentProbes DefaultProbes();

	//
	// True iff $JOURNAL_STREAM ("<device>:<inode>") parses and equals the stderr
	// device/inode. Any missing/ill-formed input yields false (fail-safe: we simply
	// do not claim journald when unsure). Pure and noexcept for exhaustive testing.
	//
	[[nodiscard]] bool JournalStreamMatches(const std::optional<std::string>& journal_stream, const std::optional<DevIno>& stderr_stat) noexcept;

	[[nodiscard]] LogEnvironment DetectLogEnvironment(const EnvironmentProbes& probes);
	[[nodiscard]] LogEnvironment DetectLogEnvironment();

	//
	// The `auto` policy (§6.2): console is always present; a journald-connected
	// stderr adds "<N>" prefixes; a Windows service adds the native Event Log sink.
	// The general native sink is NOT auto-enabled on POSIX (explicit opt-in, §4).
	// Pure and noexcept.
	//
	[[nodiscard]] SinkSelection ResolveAutoSinks(const LogEnvironment& environment, bool have_log_file) noexcept;

}
// namespace AqualinkAutomate::Logging::Sinks
