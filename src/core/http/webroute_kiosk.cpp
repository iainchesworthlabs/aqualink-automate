#include <source_location>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_kiosk.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		HTTP::Response MakeJsonResponse(unsigned version, bool keep_alive, HTTP::Status status, const nlohmann::json& body)
		{
			HTTP::Response resp{ status, version };
			resp.set(boost::beast::http::field::server, ServerFields::Server());
			resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
			resp.keep_alive(keep_alive);
			resp.body() = body.dump();
			resp.prepare_payload();
			return resp;
		}

		HTTP::Response MakeEmptyResponse(unsigned version, bool keep_alive, HTTP::Status status)
		{
			HTTP::Response resp{ status, version };
			resp.set(boost::beast::http::field::server, ServerFields::Server());
			resp.keep_alive(keep_alive);
			resp.prepare_payload();
			return resp;
		}
	}
	// anonymous namespace

	WebRoute_Kiosk::WebRoute_Kiosk(Auth::KioskService& kiosk, boost::asio::any_io_executor executor) :
		m_Kiosk(kiosk),
		m_Executor(std::move(executor))
	{
	}

	void WebRoute_Kiosk::OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Kiosk::OnRequestAsync", std::source_location::current());

		const auto version = req.version();
		const bool keep_alive = req.keep_alive();
		const auto method = req.method();

		// GET — report status (never the PIN or its hash).
		if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::ok,
				{
					{ "enabled", m_Kiosk.Enabled() },
					{ "target_group", m_Kiosk.TargetGroup() }
				}));
			return;
		}

		// DELETE — disable + clear (synchronous; no crypto).
		if (boost::beast::http::verb::delete_ == method)
		{
			m_Kiosk.ClearPin(Routing::CurrentSubject().Id, Routing::CurrentPeerIp());
			complete(MakeEmptyResponse(version, keep_alive, HTTP::Status::no_content));
			return;
		}

		// PUT — set/replace the PIN (async: argon2id on the OffloadPool).
		if (boost::beast::http::verb::put != method)
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::method_not_allowed, { { "error", "GET, PUT or DELETE required" } }));
			return;
		}

		std::string pin, target_group;

		{
			const auto body = nlohmann::json::parse(req.body(), nullptr, false);

			if (body.is_discarded() || !body.contains("pin") || !body.contains("target_group") || !body["pin"].is_string() || !body["target_group"].is_string())
			{
				complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Expected JSON body with pin and target_group" } }));
				return;
			}

			pin = body["pin"].get<std::string>();
			target_group = body["target_group"].get<std::string>();
		}

		std::string actor_id{ Routing::CurrentSubject().Id };
		std::string peer_ip{ Routing::CurrentPeerIp() };

		m_Kiosk.SetPin(std::move(pin), std::move(target_group), std::move(actor_id), std::move(peer_ip), m_Executor,
			[version, keep_alive, complete = std::move(complete)](Auth::KioskService::SetPinResult result) mutable
			{
				if (!result.Success)
				{
					complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", result.Error } }));
					return;
				}

				complete(MakeEmptyResponse(version, keep_alive, HTTP::Status::no_content));
			});
	}

	HTTP::Response WebRoute_Kiosk::OnRequest(const HTTP::Request& req)
	{
		// IsAsyncRoute() means the router never dispatches here.
		return MakeJsonResponse(req.version(), req.keep_alive(), HTTP::Status::internal_server_error, { { "error", "kiosk config is a deferred-response route" } });
	}

}
// namespace AqualinkAutomate::HTTP
