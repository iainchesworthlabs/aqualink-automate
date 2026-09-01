#include <algorithm>
#include <chrono>
#include <format>
#include <system_error>

#include "http/capture_directory.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// Convert a filesystem timestamp to seconds since the Unix epoch without
		// std::chrono::clock_cast, which is not uniformly available across the
		// standard libraries this project builds against.  Both clocks are read at
		// (effectively) the same instant, so their difference is the epoch offset.
		std::int64_t FileTimeToUnixSeconds(std::filesystem::file_time_type file_time)
		{
			const auto system_now = std::chrono::system_clock::now();
			const auto file_now = std::filesystem::file_time_type::clock::now();

			const auto system_time = system_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(file_time - file_now);

			return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
		}
	}

	CaptureDirectory::CaptureDirectory(const std::filesystem::path& root)
	{
		std::error_code ec;
		m_Root = std::filesystem::absolute(root, ec);

		if (ec)
		{
			// Fall back to the value as supplied: Jail() re-resolves per call and
			// will report the failure there rather than silently jailing to "".
			LogWarning(Channel::Web, std::format("Could not resolve capture directory '{}': {}", root.string(), ec.message()));
			m_Root = root;
		}
	}

	bool CaptureDirectory::IsAcceptableBasename(const std::string& filename, std::string& reason)
	{
		// Reject control characters and the double quote BEFORE anything else.  A
		// capture name is echoed back in the download's Content-Disposition header,
		// so a CR/LF would be header injection and a quote would break out of the
		// quoted-string; neither belongs in a filename anyway.
		for (const unsigned char c : filename)
		{
			if ((c < 0x20) || (0x7F == c) || ('"' == c))
			{
				reason = "contains a control character or quote";
				return false;
			}
		}

		// Reject any character that introduces a path on EITHER platform up front,
		// so a Windows-style traversal cannot slip through on a POSIX host (where
		// '\\' and ':' are ordinary filename characters and would not be parsed as
		// separators by std::filesystem).
		if (filename.contains('/') ||
			filename.contains('\\') ||
			filename.contains(':'))
		{
			reason = "contains a path separator or drive specifier";
			return false;
		}

		// Reject anything that is not a self-contained basename.  std::filesystem
		// is locale/separator-aware, so checking the parsed components catches both
		// POSIX ('/') and Windows ('\\', drive letters) traversal forms.
		const std::filesystem::path candidate{ filename };

		if (candidate.has_parent_path() ||      // contains a separator (a/b, /a, ../a, C:\a)
			candidate.has_root_name() ||         // drive letter / UNC root (C:, \\host)
			candidate.has_root_directory() ||    // leading separator
			candidate.filename() != candidate)   // any extra component / trailing separator
		{
			reason = "not a bare basename";
			return false;
		}

		// Defence in depth: explicitly reject the parent-directory token even though
		// has_parent_path() above already catches separated forms.
		const std::string basename = candidate.filename().string();
		if (basename == "." || basename == ".." || basename.contains(".."))
		{
			reason = "contains a parent-directory token";
			return false;
		}

		// Enforce the capture extension so neither route can be steered at, say, a
		// config or executable name that happens to sit in the capture directory.
		if (!candidate.has_extension() || candidate.extension().string() != CAPTURE_EXTENSION)
		{
			reason = std::format("must end in '{}'", CAPTURE_EXTENSION);
			return false;
		}

		return true;
	}

	std::optional<std::string> CaptureDirectory::ResolveForWrite(const std::string& filename) const
	{
		return Jail(filename, true);
	}

	std::optional<std::string> CaptureDirectory::ResolveForRead(const std::string& filename) const
	{
		return Jail(filename, false);
	}

	std::optional<std::string> CaptureDirectory::Jail(const std::string& filename, bool create_root) const
	{
		if (std::string reason; !IsAcceptableBasename(filename, reason))
		{
			LogWarning(Channel::Web, std::format("Rejected capture filename ({}): '{}'", reason, filename));
			return std::nullopt;
		}

		std::error_code ec;

		if (create_root)
		{
			// Best-effort: opening will surface any real failure to the caller.
			std::filesystem::create_directories(m_Root, ec);
			ec.clear();
		}

		// Join onto the capture directory and confirm the canonical result stays
		// inside it (belt-and-braces against any residual traversal).  Mirrors
		// HTTP::StaticFileHandler::match's path jail.
		const auto target = m_Root / std::filesystem::path{ filename }.filename();

		const auto canonical_target = std::filesystem::weakly_canonical(target, ec);
		if (ec)
		{
			LogWarning(Channel::Web, std::format("Capture path canonicalisation failed for '{}': {}", filename, ec.message()));
			return std::nullopt;
		}

		const auto canonical_root = std::filesystem::weakly_canonical(m_Root, ec);
		if (ec)
		{
			LogWarning(Channel::Web, std::format("Capture directory canonicalisation failed: {}", ec.message()));
			return std::nullopt;
		}

		// Compare on PATH COMPONENTS, not a raw string prefix: lexically_relative()
		// yields the route from the root to the resolved path, which must not be
		// empty and must not begin with a "..".
		if (const auto relative = canonical_target.lexically_relative(canonical_root);
			relative.empty() || *relative.begin() == "..")
		{
			LogWarning(Channel::Web, std::format("Rejected capture filename (escapes the capture directory): '{}'", filename));
			return std::nullopt;
		}

		return canonical_target.string();
	}

	std::vector<CaptureDirectory::Entry> CaptureDirectory::List() const
	{
		std::vector<Entry> entries;

		std::error_code ec;
		std::filesystem::directory_iterator it{ m_Root, ec };

		if (ec)
		{
			// No captures yet (the directory is created on first recording) is the
			// common case, not an error worth warning about.
			LogDebug(Channel::Web, std::format("Capture directory '{}' could not be listed: {}", m_Root.string(), ec.message()));
			return entries;
		}

		for (const auto& entry : it)
		{
			// A symlink is a way to point outside the jail; never list one (the
			// download would reject it anyway once canonicalised).
			if (std::error_code link_ec; std::filesystem::is_symlink(entry.symlink_status(link_ec)) || link_ec)
			{
				continue;
			}

			std::error_code file_ec;
			if (!entry.is_regular_file(file_ec) || file_ec)
			{
				continue;
			}

			// Only list what the download route would actually serve, so a listing
			// never advertises an entry that then answers 400.
			const auto name = entry.path().filename().string();
			if (std::string reason; !IsAcceptableBasename(name, reason))
			{
				continue;
			}

			Entry capture;
			capture.name = name;

			if (const auto size = entry.file_size(file_ec); !file_ec)
			{
				capture.bytes = size;
			}

			if (const auto written = entry.last_write_time(file_ec); !file_ec)
			{
				capture.modified_unix = FileTimeToUnixSeconds(written);
			}

			entries.push_back(std::move(capture));
		}

		// Newest first: the capture the user just stopped is the one they want.
		// Ties (same second) fall back to the name so the order is deterministic.
		std::ranges::sort(entries, [](const Entry& lhs, const Entry& rhs)
			{
				if (lhs.modified_unix != rhs.modified_unix)
				{
					return lhs.modified_unix > rhs.modified_unix;
				}

				return lhs.name < rhs.name;
			});

		return entries;
	}

}
// namespace AqualinkAutomate::HTTP
