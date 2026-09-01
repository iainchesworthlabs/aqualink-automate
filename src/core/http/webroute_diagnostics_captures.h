#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "http/capture_directory.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_CAPTURES_ROUTE_URL[] = "/api/diagnostics/recording/captures";
	inline constexpr char DIAGNOSTICS_CAPTURE_ITEM_ROUTE_URL[] = "/api/diagnostics/recording/captures/{filename}";

	/// @brief Lists the finished captures sitting in the capture directory.
	///
	/// Exists so a capture can be retrieved without a shell on the host: a packaged
	/// deployment (the Home Assistant add-on) has no interactive access to the
	/// container, which made the recording feature effectively write-only.  Only
	/// basenames are reported — never the server-side directory.
	class WebRoute_Diagnostics_Captures : public Interfaces::IWebRoute<DIAGNOSTICS_CAPTURES_ROUTE_URL>
	{
	public:
		explicit WebRoute_Diagnostics_Captures(CaptureDirectory captures);
		~WebRoute_Diagnostics_Captures() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		// Read-only diagnostics data, gated exactly like every other diagnostics
		// GET (see WebRoute_Diagnostics_Recording and friends).
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
			}

			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		CaptureDirectory m_Captures;
	};

	/// @brief Serves ONE capture file as a download.
	///
	/// The `{filename}` path parameter is client-controlled and goes through the
	/// SAME jail as the recording route's filename (CaptureDirectory), so this can
	/// never be turned into an arbitrary-file-read of the host.
	class WebRoute_Diagnostics_Capture : public Interfaces::IWebRoute<DIAGNOSTICS_CAPTURE_ITEM_ROUTE_URL>
	{
	public:
		explicit WebRoute_Diagnostics_Capture(CaptureDirectory captures);
		~WebRoute_Diagnostics_Capture() override = default;

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
		CaptureDirectory m_Captures;
	};

}
// namespace AqualinkAutomate::HTTP
