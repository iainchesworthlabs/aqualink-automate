#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_MATTER_ROUTE_URL[] = "/api/diagnostics/matter";

	// Live Matter-bridge status for the diagnostics page. The Matter stack runs in a
	// separate sidecar process (matter-bridge/), so this route proxies the sidecar's
	// status/QR endpoint (http://127.0.0.1:<status_port>/matter/status) and merges in
	// the "enabled" flag from the Matter settings. Returns
	//   { "enabled": false }
	// when Matter is opted out, and
	//   { "enabled": true, "reachable": false }
	// when enabled but the sidecar is not (yet) answering.
	//
	// The sidecar fetch is performed on a DEDICATED background thread that refreshes a
	// cached snapshot every few seconds. OnRequest only ever reads that cache, so a
	// slow/absent sidecar can NEVER stall the single-threaded main io_context loop
	// (which the serial reader, HTTP server and MQTT all cooperatively share).
	class WebRoute_Diagnostics_Matter : public Interfaces::IWebRoute<DIAGNOSTICS_MATTER_ROUTE_URL>
	{
	public:
		WebRoute_Diagnostics_Matter(bool enabled, uint16_t status_port);
		~WebRoute_Diagnostics_Matter() override;

		WebRoute_Diagnostics_Matter(const WebRoute_Diagnostics_Matter&) = delete;
		WebRoute_Diagnostics_Matter& operator=(const WebRoute_Diagnostics_Matter&) = delete;
		WebRoute_Diagnostics_Matter(WebRoute_Diagnostics_Matter&&) = delete;
		WebRoute_Diagnostics_Matter& operator=(WebRoute_Diagnostics_Matter&&) = delete;

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
			}

			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		// Background-thread body: polls the sidecar off the main loop and updates the
		// cached snapshot, sleeping between polls until signalled to stop.
		void RefreshLoop();

	private:
		const bool m_Enabled;
		const uint16_t m_StatusPort;

		// Last sidecar /matter/status body (nullopt == not reachable / not yet polled).
		mutable std::mutex m_StatusMutex;
		std::optional<std::string> m_CachedStatusBody;

		// Background refresh thread + its wake/stop signalling.
		std::thread m_RefreshThread;
		std::mutex m_WakeMutex;
		std::condition_variable m_WakeCv;
		bool m_Stop{ false };
	};

}
// namespace AqualinkAutomate::HTTP
