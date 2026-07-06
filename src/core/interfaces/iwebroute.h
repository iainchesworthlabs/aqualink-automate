#pragma once

#include <functional>
#include <string_view>

#include <boost/beast/http/verb.hpp>

#include "concepts/is_c_array.h"
#include "http/server/server_types.h"

namespace AqualinkAutomate::Interfaces
{
    // The (action, resource-kind) pair a route requires for a given HTTP method,
    // consumed by the routing layer's policy decision point (Auth::PolicyEngine)
    // when the identity system (--auth-mode) is enabled.  An empty Action means
    // "no entitlement gate" — correct only for the explicitly-open endpoints
    // (health probe, auth check, version); everything else must declare access
    // or the full-surface enforcement test fails the build.
    struct AccessRequirement
    {
        std::string_view Action{};
        std::string_view ResourceKind{};

        [[nodiscard]] constexpr bool IsSpecified() const noexcept { return !Action.empty(); }
    };

	class IWebRouteBase
    {
    public:
        IWebRouteBase() = default;
        virtual ~IWebRouteBase() = default;

    public:
        virtual std::string_view Route() const = 0;

    public:
        // Returns a mutable Response (not a type-erased message_generator) so the
        // router can stamp response-wide policy — e.g. Cache-Control: no-store on
        // dynamic API data — in one place before serialising. See HTTP_OnRequest.
        virtual HTTP::Response OnRequest(const HTTP::Request& req) = 0;

        // Per-route opt-out of the control-plane security policy. Registered routes
        // are gated by SecurityConfig (bearer token / Origin allow-list / CSRF)
        // BEFORE dispatch; a route that returns false here is dispatched WITHOUT
        // those checks. Used by the unauthenticated liveness probe (/api/health) so
        // a container/orchestrator health check can reach it without baking in the
        // operator's secret token. Defaults to true — every other route stays gated.
        virtual bool RequiresAuthentication() const { return true; }

        // The entitlement this route requires per HTTP method (typically a *.view
        // action for GET/HEAD and a control/edit action for mutating verbs).  The
        // routing layer calls the PolicyEngine with the resolved request Subject
        // when --auth-mode is enabled; with auth-mode disabled the posture rule
        // makes every decision Permit, preserving historical behaviour.  Routes
        // with per-resource grain (e.g. a specific aux) additionally refine the
        // decision in their handler where the resource id is parsed.
        virtual AccessRequirement RequiredAccess([[maybe_unused]] boost::beast::http::verb method) const { return {}; }

        // DEFERRED-RESPONSE routes (docs/auth-redesign.md §6): a handler whose
        // work must leave the kernel thread (argon2 password verification on
        // the OffloadPool) returns true here and implements OnRequestAsync;
        // the router then dispatches through the completion instead of
        // OnRequest.  RULES for async handlers: capture everything needed from
        // the request and Routing::CurrentSubject() BEFORE the first
        // suspension (both are only valid during the synchronous prefix), and
        // invoke `complete` exactly once, on the same executor the request
        // arrived on.  The default keeps every existing route synchronous.
        using AsyncCompletion = std::function<void(HTTP::Response&&)>;

        virtual bool IsAsyncRoute() const { return false; }

        virtual void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete)
        {
            complete(OnRequest(req));
        }
    };

	template<const auto& ROUTE_URL>
	requires (Concepts::CArray<decltype(ROUTE_URL)>)
    class IWebRoute : public IWebRouteBase
	{
    public:
        IWebRoute() = default;
        virtual ~IWebRoute() = default;

	public:
        virtual std::string_view Route() const final
		{
            return ROUTE_URL;
		}
	};

}
// namespace AqualinkAutomate::Interfaces
