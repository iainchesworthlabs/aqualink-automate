#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#include <cstdio>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "logging/sinks/log_environment.h"

namespace AqualinkAutomate::Logging::Sinks
{

	namespace
	{
		[[nodiscard]] std::optional<std::uint64_t> ParseUnsigned(std::string_view text) noexcept
		{
			if (text.empty())
			{
				return std::nullopt;
			}

			std::uint64_t value = 0;
			const auto* first = text.data();
			const auto* last = text.data() + text.size();

			if (const auto [ptr, ec] = std::from_chars(first, last, value); ec == std::errc{} && ptr == last)
			{
				return value;
			}

			return std::nullopt;
		}

		[[nodiscard]] bool RealStderrIsTty() noexcept
		{
#if defined(_WIN32)
			return _isatty(_fileno(stderr)) != 0;
#else
			return isatty(STDERR_FILENO) != 0;
#endif
		}

		[[nodiscard]] std::optional<std::string> RealGetEnvVar(const char* name)
		{
			if (const char* value = std::getenv(name); nullptr != value)
			{
				return std::string(value);
			}

			return std::nullopt;
		}

		[[nodiscard]] std::optional<DevIno> RealStatStderr() noexcept
		{
#if defined(_WIN32)
			// Windows has no journald and no meaningful device/inode identity for the
			// stderr handle in this sense; JOURNAL_STREAM never applies, so report
			// "unknown" and let JournalStreamMatches() fail closed.
			return std::nullopt;
#else
			struct stat st{};
			if (0 == ::fstat(STDERR_FILENO, &st))
			{
				return DevIno{ static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino) };
			}

			return std::nullopt;
#endif
		}
	}
	// namespace (anonymous)

	EnvironmentProbes DefaultProbes()
	{
		return EnvironmentProbes{
			.StderrIsTty = &RealStderrIsTty,
			.GetEnvVar = &RealGetEnvVar,
			.StatStderr = &RealStatStderr,
			// No Windows service wrapper exists yet; a future service entry point
			// replaces this probe with one that returns true in service context.
			.WindowsServiceContext = [] { return false; }
		};
	}

	bool JournalStreamMatches(const std::optional<std::string>& journal_stream, const std::optional<DevIno>& stderr_stat) noexcept
	{
		if (!journal_stream.has_value() || !stderr_stat.has_value())
		{
			return false;
		}

		const std::string_view text{ *journal_stream };
		const auto separator = text.find(':');
		if (separator == std::string_view::npos)
		{
			return false;
		}

		const auto device = ParseUnsigned(text.substr(0, separator));
		const auto inode = ParseUnsigned(text.substr(separator + 1));
		if (!device.has_value() || !inode.has_value())
		{
			return false;
		}

		return (*device == stderr_stat->Device) && (*inode == stderr_stat->Inode);
	}

	LogEnvironment DetectLogEnvironment(const EnvironmentProbes& probes)
	{
		LogEnvironment environment;

		environment.StderrIsTty = probes.StderrIsTty ? probes.StderrIsTty() : false;
		environment.StderrIsJournal = JournalStreamMatches(
			probes.GetEnvVar ? probes.GetEnvVar("JOURNAL_STREAM") : std::nullopt,
			probes.StatStderr ? probes.StatStderr() : std::nullopt);
		environment.WindowsServiceContext = probes.WindowsServiceContext ? probes.WindowsServiceContext() : false;

		return environment;
	}

	LogEnvironment DetectLogEnvironment()
	{
		return DetectLogEnvironment(DefaultProbes());
	}

	SinkSelection ResolveAutoSinks(const LogEnvironment& environment, bool have_log_file, bool journald_available) noexcept
	{
		SinkSelection selection;

		if (environment.WindowsServiceContext)
		{
			// The Event Log is the service-native trail; console stays for an
			// attached debugger / `sc` session.
			selection.Console = true;
			selection.Native = true;
		}
		else if (environment.StderrIsJournal && journald_available)
		{
			// Journal-connected AND the structured journald sink is compiled in:
			// deliver via journald natively (priority + queryable fields). The
			// console would double-log into the journal, so it is not added.
			selection.Journald = true;
		}
		else if (environment.StderrIsJournal)
		{
			// Journal-connected but no journald sink in this build: keep the console
			// and recover priority fidelity with sd-daemon "<N>" prefixes. (A syslog
			// sink here would double-log via the journal.)
			selection.Console = true;
			selection.ConsoleJournaldPrefixes = true;
		}
		else
		{
			// TTY, pipe, redirect, or container — plain console; the consumer
			// (terminal / log driver) owns storage. The general native sink is
			// explicit opt-in on POSIX and is never added by `auto`.
			selection.Console = true;
		}

		selection.File = have_log_file;

		return selection;
	}

}
// namespace AqualinkAutomate::Logging::Sinks
