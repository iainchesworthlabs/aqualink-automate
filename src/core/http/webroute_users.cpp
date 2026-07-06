#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "auth/bootstrap.h"
#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_users.h"
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

		// Parses the POST body into the create-user fields.  Returns std::nullopt on
		// success (outputs populated); on validation failure returns the error
		// Response to hand to the completion, leaving outputs unspecified.
		std::optional<HTTP::Response> ParseCreateUserBody(const HTTP::Request& req, unsigned version, bool keep_alive, std::string& username, std::string& password, std::vector<std::string>& groups, Auth::EntitlementSet& direct_entitlements)
		{
			const auto body = nlohmann::json::parse(req.body(), nullptr, false);

			if (body.is_discarded() || !body.contains("username") || !body.contains("password") || !body["username"].is_string() || !body["password"].is_string())
			{
				return MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Expected JSON body with username and password" } });
			}

			username = body["username"].get<std::string>();
			password = body["password"].get<std::string>();

			if (body.contains("groups"))
			{
				if (!body["groups"].is_array())
				{
					return MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Expected groups to be an array of group names" } });
				}

				for (const auto& entry : body["groups"])
				{
					if (!entry.is_string())
					{
						return MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Expected groups to be an array of group names" } });
					}

					groups.push_back(entry.get<std::string>());
				}
			}

			if (body.contains("direct_entitlements"))
			{
				std::string error;

				if (auto parsed = ParseEntitlementsField(body["direct_entitlements"], error); !parsed.has_value())
				{
					return MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", error } });
				}
				else
				{
					direct_entitlements = std::move(*parsed);
				}
			}

			return std::nullopt;
		}
	}
	// anonymous namespace

	WebRoute_Users::WebRoute_Users(Auth::UserStore& users, Auth::AuditLog& audit, Utility::OffloadPool& offload, Auth::PasswordHasher::Params hash_params, boost::asio::any_io_executor executor) :
		m_Users(users),
		m_Audit(audit),
		m_Offload(offload),
		m_HashParams(hash_params),
		m_Executor(std::move(executor))
	{
	}

	void WebRoute_Users::OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Users::OnRequestAsync", std::source_location::current());

		// Everything needed later is captured NOW, in the synchronous prefix:
		// the request, Routing::CurrentSubject() and Routing::CurrentPeerIp()
		// are only valid until the first suspension.
		const auto version = req.version();
		const bool keep_alive = req.keep_alive();

		if (boost::beast::http::verb::get == req.method())
		{
			nlohmann::json users = nlohmann::json::array();

			for (const auto& user : m_Users.All())
			{
				users.push_back(UserRecordToJson(user));
			}

			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::ok, users));
			return;
		}

		if (boost::beast::http::verb::post != req.method())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::method_not_allowed, { { "error", "GET or POST required" } }));
			return;
		}

		std::string username;
		std::string password;
		std::vector<std::string> groups;
		Auth::EntitlementSet direct_entitlements;

		if (auto error_response = ParseCreateUserBody(req, version, keep_alive, username, password, groups, direct_entitlements); error_response.has_value())
		{
			complete(std::move(*error_response));
			return;
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

		if (m_Users.FindByUsername(username).has_value())
		{
			// Fast-path refusal; the authoritative re-check is Create() in the
			// completion (the kernel-thread serialisation point).
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::conflict, { { "error", "Username already exists" } }));
			return;
		}

		std::string actor_id{ Routing::CurrentSubject().Id };
		std::string peer_ip{ Routing::CurrentPeerIp() };

		// Hash off-thread; create the user back on the kernel thread.
		m_Offload.Run(m_Executor,
			[password = std::move(password), params = m_HashParams]() { return Auth::PasswordHasher::Hash(password, params); },
			[this, version, keep_alive, username = std::move(username), groups = std::move(groups), direct_entitlements = std::move(direct_entitlements), actor_id = std::move(actor_id), peer_ip = std::move(peer_ip), complete = std::move(complete)](std::string password_hash) mutable
			{
				Auth::UserRecord user;
				user.Username = std::move(username);
				user.PasswordHash = std::move(password_hash);
				user.Groups = std::move(groups);
				user.DirectEntitlements = std::move(direct_entitlements);

				if (std::string error; !m_Users.Create(user, error))
				{
					complete(MakeJsonResponse(version, keep_alive, StatusForStoreError(error), { { "error", error } }));
					return;
				}

				// Create() generated the id; re-read the record for the response.
				const auto created = m_Users.FindByUsername(user.Username);

				m_Audit.Record(MakeAdminAuditEvent(actor_id, "auth.user_created", "user", created.has_value() ? created->Id : "", peer_ip, user.Username));

				complete(MakeJsonResponse(version, keep_alive, HTTP::Status::created, created.has_value() ? UserRecordToJson(*created) : nlohmann::json{ { "username", user.Username } }));
			});
	}

	HTTP::Response WebRoute_Users::OnRequest(const HTTP::Request& req)
	{
		// IsAsyncRoute() means the router never dispatches here.
		return MakeJsonResponse(req.version(), req.keep_alive(), HTTP::Status::internal_server_error, { { "error", "users is a deferred-response route" } });
	}

}
// namespace AqualinkAutomate::HTTP
