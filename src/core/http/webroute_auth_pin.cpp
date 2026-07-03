#include <source_location>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_auth_pin.h"
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
	}
	// anonymous namespace

	WebRoute_AuthPin::WebRoute_AuthPin(Auth::KioskService& kiosk, boost::asio::any_io_executor executor) :
		m_Kiosk(kiosk),
		m_Executor(std::move(executor))
	{
	}

	void WebRoute_AuthPin::OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_AuthPin::OnRequestAsync", std::source_location::current());

		// Capture everything needed in the synchronous prefix: req and
		// Routing::CurrentPeerIp() are only valid until the first suspension.
		const auto version = req.version();
		const bool keep_alive = req.keep_alive();

		if (boost::beast::http::verb::post != req.method())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::method_not_allowed, { { "error", "POST required" } }));
			return;
		}

		std::string pin;

		{
			const auto body = nlohmann::json::parse(req.body(), nullptr, false);

			if (body.is_discarded() || !body.contains("pin") || !body["pin"].is_string())
			{
				complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Expected JSON body with a pin" } }));
				return;
			}

			pin = body["pin"].get<std::string>();
		}

		std::string peer_ip{ Routing::CurrentPeerIp() };

		const auto ua_it = req.find(boost::beast::http::field::user_agent);
		std::string user_agent = (req.end() != ua_it) ? std::string{ ua_it->value() } : std::string{};

		m_Kiosk.LoginWithPin(std::move(pin), std::move(peer_ip), std::move(user_agent), m_Executor,
			[version, keep_alive, complete = std::move(complete)](Auth::KioskService::LoginResult result) mutable
			{
				if (result.LockedOut)
				{
					auto resp = MakeJsonResponse(version, keep_alive, HTTP::Status::too_many_requests, { { "error", result.Error } });
					resp.set(boost::beast::http::field::retry_after, "900");
					complete(std::move(resp));
					return;
				}

				if (!result.Success)
				{
					complete(MakeJsonResponse(version, keep_alive, HTTP::Status::unauthorized, { { "error", result.Error } }));
					return;
				}

				complete(MakeJsonResponse(version, keep_alive, HTTP::Status::ok,
					{
						{ "access_token", result.AccessToken },
						{ "refresh_token", result.RefreshToken },
						{ "session_id", result.SessionId },
						{ "token_type", "Bearer" }
					}));
			});
	}

	HTTP::Response WebRoute_AuthPin::OnRequest(const HTTP::Request& req)
	{
		// IsAsyncRoute() means the router never dispatches here.
		return MakeJsonResponse(req.version(), req.keep_alive(), HTTP::Status::internal_server_error, { { "error", "pin login is a deferred-response route" } });
	}

}
// namespace AqualinkAutomate::HTTP
