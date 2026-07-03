#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <string>

#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/logging.h"
#include "logging/logging_formatter.h"
#include "logging/sinks/sink_file.h"
#include "logging/sinks/sink_filters.h"

namespace AqualinkAutomate::Logging::Sinks
{

	namespace
	{
		namespace sinks = boost::log::sinks;
		namespace keywords = boost::log::keywords;
	}

	boost::shared_ptr<boost::log::sinks::sink> MakeFileSink(const FileSinkConfig& config)
	{
		namespace fs = std::filesystem;

		try
		{
			const fs::path dir = config.Path.has_parent_path() ? config.Path.parent_path() : fs::path{ "." };
			const std::string stem = config.Path.stem().string();
			const std::string ext = config.Path.extension().string();

			// Rotated files: "<stem>_<5-digit counter><ext>", kept in the same dir.
			const std::string rotated_pattern = stem + "_%5N" + ext;

			auto backend = boost::make_shared<sinks::text_file_backend>(
				keywords::file_name = config.Path.string(),
				keywords::target_file_name = rotated_pattern,
				keywords::rotation_size = config.MaxFileBytes,
				keywords::auto_flush = true);

			// Bound the total rotated files (deletes the oldest beyond MaxFiles).
			backend->set_file_collector(sinks::file::make_collector(
				keywords::target = dir.string(),
				keywords::max_files = static_cast<std::uintmax_t>(config.MaxFiles)));
			backend->scan_for_files();

			// Synchronous frontend with per-record flush (auto_flush above) — the same
			// tradeoff the console and native sinks make. An asynchronous frontend (so a
			// slow disk cannot stall the kernel thread) is a deferred refinement: it needs
			// the remove->stop->flush teardown ordering wired through SinkRegistry, which
			// currently owns base sink handles that expose no stop().
			using file_sink = sinks::synchronous_sink<sinks::text_file_backend>;
			auto sink = boost::make_shared<file_sink>(backend);

			sink->set_filter(MakeOperationalFilter());

			const bool as_json = (config.Format == LogFormat::Json);
			sink->set_formatter([as_json](boost::log::record_view const& rec, boost::log::formatting_ostream& strm)
				{
					if (as_json)
					{
						JsonFormatter(rec, strm);
					}
					else
					{
						Formatter(rec, strm);
					}
				});

			return sink;
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, std::format("Could not open the log file '{}' ({}); continuing without the file sink", config.Path.string(), ex.what()));
			return {};
		}
	}

}
// namespace AqualinkAutomate::Logging::Sinks
