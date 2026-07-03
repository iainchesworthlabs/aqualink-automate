#include <source_location>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "auth/bootstrap.h"
#include "http/server/server_fields.h"
#include "http/webroute_auth_setup.h"
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

	WebRoute_AuthSetup::WebRoute_AuthSetup(Auth::UserStore& users, Auth::AuditLog& audit, Utility::OffloadPool& offload, Auth::PasswordHasher::Params hash_params, boost::asio::any_io_executor executor) :
		m_Users(users),
		m_Audit(audit),
		m_Offload(offload),
		m_HashParams(hash_params),
		m_Executor(std::move(executor))
	{
	}

	void WebRoute_AuthSetup::OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_AuthSetup::OnRequestAsync", std::source_location::current());

		const auto version = req.version();
		const bool keep_alive = req.keep_alive();

		if (boost::beast::http::verb::post != req.method())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::method_not_allowed, { { "error", "POST required" } }));
			return;
		}

		if (!m_Users.Empty())
		{
			// Self-sealing: once the system has an owner, setup is gone.
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::forbidden, { { "error", "Setup has already been completed" } }));
			return;
		}

		std::string username, password;

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

		if (username.empty())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Username is required" } }));
			return;
		}

		if (const auto policy_error = Auth::ValidatePasswordPolicy(password); policy_error.has_value())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", *policy_error } }));
			return;
		}

		// Hash off-thread; create the admin back on the kernel thread (the
		// serialisation point for the emptiness re-check).
		m_Offload.Run(m_Executor,
			[password = std::move(password), params = m_HashParams]() { return Auth::PasswordHasher::Hash(password, params); },
			[this, version, keep_alive, username = std::move(username), complete = std::move(complete)](std::string password_hash) mutable
			{
				std::string error;

				const auto user_id = Auth::BootstrapAdminWithHash(m_Users, username, std::move(password_hash), m_Audit, error);

				if (!user_id.has_value())
				{
					const auto status = ("Setup has already been completed" == error) ? HTTP::Status::forbidden : HTTP::Status::bad_request;
					complete(MakeJsonResponse(version, keep_alive, status, { { "error", error } }));
					return;
				}

				complete(MakeJsonResponse(version, keep_alive, HTTP::Status::created,
					{
						{ "user_id", *user_id },
						{ "username", username }
					}));
			});
	}

	HTTP::Response WebRoute_AuthSetup::OnRequest(const HTTP::Request& req)
	{
		// IsAsyncRoute() means the router never dispatches here.
		return MakeJsonResponse(req.version(), req.keep_alive(), HTTP::Status::internal_server_error, { { "error", "setup is a deferred-response route" } });
	}

}
// namespace AqualinkAutomate::HTTP
