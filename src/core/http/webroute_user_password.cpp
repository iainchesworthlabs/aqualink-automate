#include <source_location>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "auth/bootstrap.h"
#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_user_password.h"
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

	WebRoute_UserPassword::WebRoute_UserPassword(Auth::UserStore& users, Auth::GroupStore& groups, Auth::SessionStore& sessions, Auth::AuditLog& audit, Utility::OffloadPool& offload, Auth::PasswordHasher::Params hash_params, boost::asio::any_io_executor executor) :
		m_Users(users),
		m_Groups(groups),
		m_Sessions(sessions),
		m_Audit(audit),
		m_Offload(offload),
		m_HashParams(hash_params),
		m_Executor(std::move(executor))
	{
	}

	void WebRoute_UserPassword::OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_UserPassword::OnRequestAsync", std::source_location::current());

		// Everything needed later is captured NOW, in the synchronous prefix:
		// the request, Routing::CurrentSubject() and Routing::CurrentPeerIp()
		// are only valid until the first suspension.
		const auto version = req.version();
		const bool keep_alive = req.keep_alive();

		if (boost::beast::http::verb::put != req.method())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::method_not_allowed, { { "error", "PUT required" } }));
			return;
		}

		const auto user_id = AdminRoutePathParam(req);

		if (!user_id.has_value())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::not_found, { { "error", "User not found" } }));
			return;
		}

		// The admin/self rule (see the class comment): a non-admin subject may
		// only set their OWN password.
		const auto& subject = Routing::CurrentSubject();

		if (const bool is_admin = subject.Entitlements.Permits(Auth::Vocabulary::SYSTEM_ADMIN); !is_admin && (subject.Id != *user_id))
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::forbidden, { { "error", "You may only change your own password" } }));
			return;
		}

		if (!m_Users.FindById(*user_id).has_value())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::not_found, { { "error", "User not found" } }));
			return;
		}

		std::string password;

		{
			const auto body = nlohmann::json::parse(req.body(), nullptr, false);

			if (body.is_discarded() || !body.contains("password") || !body["password"].is_string())
			{
				complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", "Expected JSON body with password" } }));
				return;
			}

			password = body["password"].get<std::string>();
		}

		if (const auto policy_error = Auth::ValidatePasswordPolicy(password); policy_error.has_value())
		{
			complete(MakeJsonResponse(version, keep_alive, HTTP::Status::bad_request, { { "error", *policy_error } }));
			return;
		}

		std::string actor_id{ subject.Id };
		std::string peer_ip{ Routing::CurrentPeerIp() };

		// Hash off-thread; apply the change back on the kernel thread.
		m_Offload.Run(m_Executor,
			[password = std::move(password), params = m_HashParams]() { return Auth::PasswordHasher::Hash(password, params); },
			[this, version, keep_alive, user_id = *user_id, actor_id = std::move(actor_id), peer_ip = std::move(peer_ip), complete = std::move(complete)](std::string password_hash) mutable
			{
				// Re-read: the record may have changed during the hash.
				auto user = m_Users.FindById(user_id);

				if (!user.has_value())
				{
					complete(MakeJsonResponse(version, keep_alive, HTTP::Status::not_found, { { "error", "User not found" } }));
					return;
				}

				user->PasswordHash = std::move(password_hash);

				if (std::string error; !m_Users.Update(*user, m_Groups.Registry(), error))
				{
					complete(MakeJsonResponse(version, keep_alive, StatusForStoreError(error), { { "error", error } }));
					return;
				}

				// A password change invalidates EVERYTHING (§6): tokver bump
				// kills outstanding access tokens, the session sweep kills
				// every refresh token.
				m_Users.BumpTokenVersion(user_id);
				m_Sessions.RevokeAllForUser(user_id);

				m_Audit.Record(MakeAdminAuditEvent(actor_id, "auth.password_changed", "user", user_id, peer_ip));

				HTTP::Response resp{ HTTP::Status::no_content, version };
				resp.set(boost::beast::http::field::server, ServerFields::Server());
				resp.keep_alive(keep_alive);
				resp.prepare_payload();
				complete(std::move(resp));
			});
	}

	HTTP::Response WebRoute_UserPassword::OnRequest(const HTTP::Request& req)
	{
		// IsAsyncRoute() means the router never dispatches here.
		return MakeJsonResponse(req.version(), req.keep_alive(), HTTP::Status::internal_server_error, { { "error", "password is a deferred-response route" } });
	}

}
// namespace AqualinkAutomate::HTTP
