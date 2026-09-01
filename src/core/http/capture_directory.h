#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AqualinkAutomate::HTTP
{

	/// @brief The on-disk directory that on-demand serial captures are confined to.
	///
	/// This is the SOLE location the diagnostics recording route is permitted to
	/// write to, and the SOLE location the capture-download route is permitted to
	/// read from.  The root is OPERATOR-supplied (`--capture-directory`, default
	/// `<cwd>/captures`); the filenames that index into it are CLIENT-supplied and
	/// therefore untrusted, so every filename is jailed before it reaches any
	/// file-opening code.
	///
	/// Making the root configurable is what lets a packaged deployment put captures
	/// somewhere the user can actually reach (the Home Assistant add-on points it at
	/// its user-browsable app_config share) without core code knowing anything about
	/// that deployment.
	class CaptureDirectory
	{
	public:
		/// @brief One capture file in the directory, as reported to a client.
		struct Entry
		{
			std::string name;                 ///< Bare basename (never a path).
			std::uintmax_t bytes{ 0 };        ///< File size in bytes.
			std::int64_t modified_unix{ 0 };  ///< Last-write time, seconds since the Unix epoch.
		};

		/// @brief Required extension for capture files (rejects e.g. ".cap" -> ".conf").
		static constexpr std::string_view CAPTURE_EXTENSION{ ".cap" };

	public:
		/// @brief Bind to @p root, resolved to an absolute path.
		///
		/// The directory is NOT created here — it is created on demand when a
		/// recording is started, so an install that never records leaves no
		/// directory behind.
		explicit CaptureDirectory(const std::filesystem::path& root);

		/// @brief The absolute capture root.  Never sent to a client.
		const std::filesystem::path& Root() const noexcept { return m_Root; }

		/// @brief Jail @p filename for WRITING a new capture (creates the root).
		/// @returns the safe absolute path to open, or std::nullopt (reason logged)
		///          when the filename is rejected.
		std::optional<std::string> ResolveForWrite(const std::string& filename) const;

		/// @brief Jail @p filename for READING an existing capture (never creates).
		/// @returns the safe absolute path to open, or std::nullopt (reason logged)
		///          when the filename is rejected.  Existence is NOT checked — the
		///          caller decides how a missing file is reported.
		std::optional<std::string> ResolveForRead(const std::string& filename) const;

		/// @brief The capture files currently in the directory, newest first.
		///
		/// Only regular files whose names pass the same basename rules as the jail
		/// are listed, so every entry returned here is downloadable.  Symlinks are
		/// skipped outright (a link is a way to point outside the jail).  A missing
		/// or unreadable directory yields an empty list, not an error.
		std::vector<Entry> List() const;

	public:
		/// @brief True when @p filename is a self-contained, jailable capture basename.
		///
		/// Rejects path separators (either platform's), drive letters, leading
		/// separators, any parent-directory token, and anything not ending in
		/// CAPTURE_EXTENSION.  Pure string/lexical work: performs no filesystem
		/// access, so it is equally usable for validating input and for filtering a
		/// directory listing.
		///
		/// @param reason  Set to a one-line rejection reason when false is returned.
		static bool IsAcceptableBasename(const std::string& filename, std::string& reason);

	private:
		/// @brief Shared implementation of ResolveForWrite/ResolveForRead.
		std::optional<std::string> Jail(const std::string& filename, bool create_root) const;

	private:
		// Absolute, but deliberately NOT canonicalised at construction: the root may
		// not exist yet, and canonicalising it before it does would cache a form that
		// no longer matches once the directory (or a symlink on its path) is created.
		// Jail() canonicalises root and target together, per call.
		std::filesystem::path m_Root;
	};

}
// namespace AqualinkAutomate::HTTP
