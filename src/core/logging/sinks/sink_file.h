#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <boost/log/sinks/sink.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/logging_formatter.h"

namespace AqualinkAutomate::Logging::Sinks
{

	struct FileSinkConfig
	{
		// Active log file. Rotated files are written alongside it as
		// "<stem>_<NNNNN><ext>", bounded by MaxFiles.
		std::filesystem::path Path;

		// Rotate when the active file would exceed this many bytes.
		std::uintmax_t MaxFileBytes = 10ull * 1024ull * 1024ull;

		// Keep at most this many rotated files (the collector deletes the oldest).
		std::size_t MaxFiles = 5;

		// Text (human) or JSON-lines (pipelines).
		LogFormat Format = LogFormat::Text;
	};

	//
	// Build (but do NOT install) the operational file sink: a Boost text_file_backend
	// with size rotation and a bounded collector. Carries the shared operational filter
	// (audit never reaches it). Returns null — after a warning — if the file cannot be
	// opened. Synchronous frontend for now (an async frontend is a deferred refinement;
	// see the note in sink_file.cpp).
	//
	[[nodiscard]] boost::shared_ptr<boost::log::sinks::sink> MakeFileSink(const FileSinkConfig& config);

}
// namespace AqualinkAutomate::Logging::Sinks
