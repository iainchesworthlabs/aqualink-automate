// journald is a Linux concept; libsystemd is resolved at RUNTIME via dlopen (no
// build-time libsystemd-dev, no link dependency, no packaging/runtime coordination).
// If the shared library is absent, IsJournaldAvailable() is false and the sink is
// simply never selected (the auto policy falls back to console + "<N>" prefixes).
#if defined(__linux__)

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <sys/uio.h>            // struct iovec

#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <magic_enum/magic_enum.hpp>

#include "logging/logging_attributes.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/severity_mappings.h"
#include "logging/sinks/sink_journald.h"

namespace AqualinkAutomate::Logging::Sinks
{

	namespace
	{
		// Signature of systemd's sd_journal_sendv (declared here so we need no
		// <systemd/sd-journal.h> at build time).
		using SdJournalSendvFn = int (*)(const struct iovec*, int);

		// Resolve sd_journal_sendv once by dlopen'ing the versioned soname. The handle
		// is intentionally never dlclose'd — the sink lives for the process lifetime,
		// and (unlike a boost::log syslog_backend over boost::asio) this owns no
		// global execution-context state to tear down at exit.
		SdJournalSendvFn ResolveSendv()
		{
			static SdJournalSendvFn resolved = []() -> SdJournalSendvFn
			{
				void* handle = ::dlopen("libsystemd.so.0", RTLD_LAZY | RTLD_GLOBAL);
				if (nullptr == handle)
				{
					return nullptr;
				}

				return reinterpret_cast<SdJournalSendvFn>(::dlsym(handle, "sd_journal_sendv"));
			}();

			return resolved;
		}

		//
		// Custom Boost.Log backend: forwards each record to the journal via the
		// resolved sd_journal_sendv with structured fields.
		//
		class JournaldBackend : public boost::log::sinks::basic_formatted_sink_backend<char>
		{
		public:
			explicit JournaldBackend(std::string identifier) :
				m_Identifier(std::move(identifier))
			{
			}

			void consume(boost::log::record_view const& rec, string_type const& formatted_message)
			{
				const auto send = ResolveSendv();
				if (nullptr == send)
				{
					return;  // libsystemd vanished mid-run; nothing sensible to do
				}

				const auto record_severity = rec[severity].get<Severity>();

				// Each field is a "NAME=value" string; sd_journal_sendv takes them as an
				// iovec array, which lets us include CODE_FILE/CODE_LINE conditionally.
				std::vector<std::string> fields;
				fields.reserve(6);
				fields.emplace_back("MESSAGE=" + formatted_message);
				fields.emplace_back("PRIORITY=" + std::to_string(SyslogPriorityValue(record_severity)));
				fields.emplace_back("SYSLOG_IDENTIFIER=" + m_Identifier);

				if (const auto record_channel = rec[channel])
				{
					fields.emplace_back(std::string("AA_CHANNEL=") + std::string(magic_enum::enum_name(record_channel.get())));
				}

				if (Severity::Trace == record_severity || Severity::Debug == record_severity)
				{
					if (const auto file = rec[source_file])
					{
						fields.emplace_back("CODE_FILE=" + file.get());
					}
					if (const auto line = rec[source_line])
					{
						fields.emplace_back("CODE_LINE=" + std::to_string(line.get()));
					}
				}

				std::vector<iovec> iov;
				iov.reserve(fields.size());
				for (auto& field : fields)
				{
					iov.push_back(iovec{ field.data(), field.size() });
				}

				send(iov.data(), static_cast<int>(iov.size()));
			}

		private:
			std::string m_Identifier;
		};
	}
	// namespace (anonymous)

	bool IsJournaldAvailable()
	{
		return nullptr != ResolveSendv();
	}

	boost::shared_ptr<boost::log::sinks::sink> MakeJournaldSink(const JournaldSinkConfig& config)
	{
		namespace expr = boost::log::expressions;

		if (!IsJournaldAvailable())
		{
			return {};
		}

		using journald_sink = boost::log::sinks::synchronous_sink<JournaldBackend>;
		auto sink = boost::make_shared<journald_sink>(boost::make_shared<JournaldBackend>(config.SyslogIdentifier));

		sink->set_filter(config.Filter);
		// The MESSAGE field is the formatted text; the structured fields carry the rest.
		sink->set_formatter(expr::stream << expr::smessage);

		return sink;
	}

}
// namespace AqualinkAutomate::Logging::Sinks

#endif // __linux__
