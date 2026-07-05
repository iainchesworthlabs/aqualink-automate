#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

#include "http/webroute_metrics.h"
#include "http/server/server_fields.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "version/version_cmake.h"
#include "version/version_git.h"

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		// Prometheus text exposition format, version 0.0.4.
		constexpr std::string_view METRICS_CONTENT_TYPE{ "text/plain; version=0.0.4; charset=utf-8" };

		// Escape a Prometheus label value per the exposition spec: backslash,
		// double-quote and newline are the only characters that must be escaped.
		std::string EscapeLabelValue(std::string_view value)
		{
			std::string out;
			out.reserve(value.size());
			for (const char c : value)
			{
				switch (c)
				{
				case '\\': out += "\\\\"; break;
				case '"':  out += "\\\""; break;
				case '\n': out += "\\n";  break;
				default:   out += c;      break;
				}
			}
			return out;
		}

		// Emit a single-sample counter (HELP + TYPE + value).
		void AppendCounter(std::string& out, std::string_view name, std::string_view help, uint64_t value)
		{
			out += "# HELP "; out += name; out += ' '; out += help; out += '\n';
			out += "# TYPE "; out += name; out += " counter\n";
			out += name; out += ' '; out += std::to_string(value); out += '\n';
		}

		// Emit the HELP + TYPE preamble for a metric family with multiple labelled
		// samples (the caller appends the individual sample lines).
		void AppendFamilyHeader(std::string& out, std::string_view name, std::string_view help, std::string_view type)
		{
			out += "# HELP "; out += name; out += ' '; out += help; out += '\n';
			out += "# TYPE "; out += name; out += ' '; out += type; out += '\n';
		}

		void AppendLabelledCounter(std::string& out, std::string_view name, std::string_view label, std::string_view label_value, uint64_t value)
		{
			out += name; out += '{'; out += label; out += "=\""; out += label_value; out += "\"} "; out += std::to_string(value); out += '\n';
		}

		// Render a double for Prometheus consumption.  std::to_string would force a
		// fixed 6-decimal form; the chrono->microsecond conversions below are exact
		// to the nanosecond, so format with up to 3 decimals and trim.
		std::string FormatDouble(double value)
		{
			char buf[32];
			auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, 3);
			if (ec != std::errc{})
			{
				return "0";
			}
			std::string_view sv{ buf, static_cast<std::size_t>(ptr - buf) };
			// Trim trailing zeros / dangling decimal point for a tidy "12.5" / "0".
			std::size_t end = sv.size();
			if (sv.find('.') != std::string_view::npos)
			{
				while (end > 0 && sv[end - 1] == '0') { --end; }
				if (end > 0 && sv[end - 1] == '.') { --end; }
			}
			return std::string{ sv.substr(0, end) };
		}

		double ToMicroseconds(std::chrono::nanoseconds ns)
		{
			return static_cast<double>(ns.count()) / 1000.0;
		}

		// Append the four tracked quantiles for one latency stage.
		void AppendLatencyStage(std::string& out, std::string_view stage, const Utility::LatencyPercentileTracker<>& tracker)
		{
			const auto snapshot = tracker.GetSnapshot();

			struct Quantile { std::string_view label; std::chrono::nanoseconds value; };
			const std::array<Quantile, 4> quantiles
			{ {
				{ "0.01", snapshot.p1  },
				{ "0.5",  snapshot.p50 },
				{ "0.95", snapshot.p95 },
				{ "0.99", snapshot.p99 },
			} };

			for (const auto& q : quantiles)
			{
				out += "aqualink_latency_microseconds{stage=\"";
				out += stage;
				out += "\",quantile=\"";
				out += q.label;
				out += "\"} ";
				out += FormatDouble(ToMicroseconds(q.value));
				out += '\n';
			}
		}
	}
	// unnamed namespace

	WebRoute_Metrics::WebRoute_Metrics(Kernel::HubLocator& hub_locator) :
		Interfaces::IWebRoute<METRICS_ROUTE_URL>(),
		m_StatisticsHub(hub_locator.Find<Kernel::StatisticsHub>())
	{
	}

	HTTP::Response WebRoute_Metrics::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Metrics::OnRequest", std::source_location::current());

		std::string body;
		body.reserve(2048);

		// --- Build info (info-style gauge, always 1) ----------------------------
		{
			const std::string version = EscapeLabelValue(Version::VersionInfo::ProjectVersionFull());
			const std::string commit = EscapeLabelValue(Version::GitMetadata::Populated() ? Version::GitMetadata::CommitSHA1() : std::string{});

			body += "# HELP aqualink_build_info Build information (constant 1; value carried in labels).\n";
			body += "# TYPE aqualink_build_info gauge\n";
			body += "aqualink_build_info{version=\"";
			body += version;
			body += "\",commit=\"";
			body += commit;
			body += "\"} 1\n";
		}

		if (m_StatisticsHub)
		{
			const auto& stats = *m_StatisticsHub;

			// --- Serial byte counters -------------------------------------------
			AppendCounter(body, "aqualink_serial_read_bytes_total", "Total bytes read from the serial port.", stats.BandwidthMetrics.Read.TotalBytes);
			AppendCounter(body, "aqualink_serial_write_bytes_total", "Total bytes written to the serial port.", stats.BandwidthMetrics.Write.TotalBytes);

			// --- Total decoded messages -----------------------------------------
			uint64_t message_total = 0;
			for (const auto& entry : stats.MessageCounts)
			{
				message_total += entry.second.Count();
			}
			AppendCounter(body, "aqualink_messages_total", "Total protocol messages decoded across all message types.", message_total);

			// --- Message error counters -----------------------------------------
			AppendFamilyHeader(body, "aqualink_message_errors_total", "Protocol message decode errors by kind.", "counter");
			AppendLabelledCounter(body, "aqualink_message_errors_total", "kind", "checksum", stats.MessageErrors.ChecksumFailures);
			AppendLabelledCounter(body, "aqualink_message_errors_total", "kind", "deserialization", stats.MessageErrors.DeserializationFailures);
			AppendLabelledCounter(body, "aqualink_message_errors_total", "kind", "invalid_packet_format", stats.MessageErrors.InvalidPacketFormat);
			AppendLabelledCounter(body, "aqualink_message_errors_total", "kind", "generator", stats.MessageErrors.GeneratorFailures);
			AppendLabelledCounter(body, "aqualink_message_errors_total", "kind", "overlapping_packets", stats.MessageErrors.OverlappingPackets);
			AppendLabelledCounter(body, "aqualink_message_errors_total", "kind", "buffer_overflow", stats.MessageErrors.BufferOverflows);

			// --- Serial-layer error counters ------------------------------------
			AppendFamilyHeader(body, "aqualink_serial_errors_total", "Serial-layer errors by kind.", "counter");
			AppendLabelledCounter(body, "aqualink_serial_errors_total", "kind", "overflow", stats.Serial.SerialOverflowCount);
			AppendLabelledCounter(body, "aqualink_serial_errors_total", "kind", "underflow", stats.Serial.SerialUnderflowCount);
			AppendLabelledCounter(body, "aqualink_serial_errors_total", "kind", "transmission_failure", stats.Serial.TransmissionFailures);

			// --- Latency percentile gauges --------------------------------------
			AppendFamilyHeader(body, "aqualink_latency_microseconds", "Operation latency percentiles (sliding 60s window), in microseconds.", "gauge");
			AppendLatencyStage(body, "serial_read", stats.LatencyMetrics.ReadLatency);
			AppendLatencyStage(body, "serial_write", stats.LatencyMetrics.WriteLatency);
			AppendLatencyStage(body, "msg_proc", stats.LatencyMetrics.MessageProcessingLatency);
		}

		HTTP::Response resp{ HTTP::Status::ok, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.set(boost::beast::http::field::content_type, METRICS_CONTENT_TYPE);
		resp.keep_alive(req.keep_alive());
		resp.body() = std::move(body);
		resp.prepare_payload();

		zone->Value(resp.body().size());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
