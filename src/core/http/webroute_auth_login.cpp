#include <functional>
#include <source_location>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_auth_login.h"
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

		void CompleteLogin(unsigned version, bool keep_alive, std::function<void(HTTP::Response&&)>& complete, Auth::SessionService::LoginResult result)
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
		}
	}
	// anonymous namespace

	WebRoute_AuthLogin::WebRoute_AuthLogin(Auth::SessionService& sessions, boost::asio::any_io_executor executor) :
		m_Sessions(sessions),
		m_Executor(std::move(executor))
	{
	}

	void WebRoute_AuthLogin::OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_AuthLogin::OnRequestAsync", std::source_location::current());

		// Everything needed later is captured NOW, in the synchronous prefix:
		// the request and Routing::CurrentPeerIp() are only valid until the
		// first suspension.
		const auto version = req.version();
		const bool keep_alive = req.keep_alive();

		if (boost::beast::http::verb::post != req.method())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::method_not_allowed, { { "error", "POST required" } }));
			return;
		}

		std::string username;
		std::string password;

		{
			const auto body = nlohmann::json::parse(req.body(), nullptr, false);

			if (body.is_discarded() || !body.contains("username") || !body.contains("password") || !body["username"].is_string() || !body["password"].is_string())
			{
				complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Expected JSON body with username and password" } }));
				return;
			}

			username = body["username"].get<std::string>();
			password = body["password"].get<std::string>();
		}

		std::string peer_ip{ Routing::CurrentPeerIp() };

		const auto ua_it = req.find(boost::beast::http::field::user_agent);
		std::string user_agent = (req.end() != ua_it) ? std::string{ ua_it->value() } : std::string{};

		m_Sessions.Login(std::move(username), std::move(password), std::move(peer_ip), std::move(user_agent), m_Executor,
			[version, keep_alive, complete = std::move(complete)](Auth::SessionService::LoginResult result) mutable
			{
				CompleteLogin(version, keep_alive, complete, std::move(result));
			});
	}

	HTTP::Response WebRoute_AuthLogin::OnRequest(const HTTP::Request& req)
	{
		// IsAsyncRoute() means the router never dispatches here.
		return MakeJsonResponse(req.version(), req.keep_alive(), HTTP::Status::internal_server_error, { { "error", "login is a deferred-response route" } });
	}

}
// namespace AqualinkAutomate::HTTP
