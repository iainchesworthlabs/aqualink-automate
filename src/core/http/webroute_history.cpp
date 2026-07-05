#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "history/history_service.h"
#include "http/server/parse_query_string.h"
#include "http/server/responses/response_400.h"
#include "http/server/responses/response_404.h"
#include "http/server/responses/response_503.h"
#include "http/server/server_fields.h"
#include "http/webroute_history.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		constexpr int DEFAULT_MAX_POINTS = 500;
		constexpr int MAX_MAX_POINTS = 2000;

		std::optional<std::int64_t> ParseInt64Query(const HTTP::Request& req, const std::string& name)
		{
			auto value = HTTP::ParseQueryString(req, name);
			if (!value.has_value())
			{
				return std::nullopt;
			}
			const std::string& text = value.value();
			std::int64_t out = 0;
			const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
			if (ec != std::errc{} || ptr != text.data() + text.size())
			{
				return std::nullopt;
			}
			return out;
		}

		HTTP::Response JsonResponse(const HTTP::Request& req, const nlohmann::json& body)
		{
			HTTP::Response resp{ HTTP::Status::ok, req.version() };
			resp.set(boost::beast::http::field::server, ServerFields::Server());
			resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
			resp.keep_alive(req.keep_alive());
			resp.body() = body.dump();
			resp.prepare_payload();
			return resp;
		}
	}
	// unnamed namespace

	WebRoute_History::WebRoute_History(std::shared_ptr<History::HistoryService> service) :
		m_Service(std::move(service))
	{
	}

	HTTP::Response WebRoute_History::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_History::OnRequest", std::source_location::current());

		// History disabled (no --history-db) -> 503, self-describing.
		if (!m_Service)
		{
			return HTTP::Responses::Response_503(req);
		}

		auto key_query = HTTP::ParseQueryString(req, "key");

		// No key -> list all series with their metadata.
		if (!key_query.has_value())
		{
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& series : m_Service->ListSeries())
			{
				arr.push_back({
					{ "key", series.key },
					{ "unit", series.unit },
					{ "label", series.label },   // friendly name for device series; "" otherwise
					{ "first_ts", series.first_ts },
					{ "last_ts", series.last_ts },
					{ "count", series.count },
				});
			}
			return JsonResponse(req, arr);
		}

		const std::string key = key_query.value();
		if (!m_Service->SeriesExists(key))
		{
			return HTTP::Responses::Response_404(req);
		}

		// from/to default to a wide range so a bare key request still returns data.
		const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		const std::int64_t from = ParseInt64Query(req, "from").value_or(0);
		const std::int64_t to = ParseInt64Query(req, "to").value_or(now);

		if (to < from)
		{
			return HTTP::Responses::Response_400(req);
		}

		auto max_points = static_cast<int>(ParseInt64Query(req, "max_points").value_or(DEFAULT_MAX_POINTS));
		max_points = std::clamp(max_points, 1, MAX_MAX_POINTS);

		nlohmann::json points = nlohmann::json::array();
		for (const auto& point : m_Service->QuerySeries(key, from, to, max_points))
		{
			points.push_back({ { "ts", point.ts }, { "value", point.value } });
		}

		nlohmann::json body = {
			{ "key", key },
			{ "from", from },
			{ "to", to },
			{ "max_points", max_points },
			{ "points", std::move(points) },
		};
		return JsonResponse(req, body);
	}

}
// namespace AqualinkAutomate::HTTP
