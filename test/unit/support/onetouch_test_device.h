#pragma once

#include <cstdint>
#include <string>

#include "jandy/devices/onetouch_device.h"

namespace AqualinkAutomate::Test
{

	// Test-only subclass that exposes the drivers the OneTouch unit tests need: forcing the fault
	// operating states, reading whether the device recovered to NormalOperation, presenting a screen
	// line / cursor as an incoming frame would, and running one real controller update. These used
	// to be protected "*ForTest" seams ON OneTouchDevice; moving them onto this subclass keeps the
	// production class free of test-only methods (it just makes the members they touch protected).
	//
	// The tests derive their own device types from this, so their existing `device.X()` call sites
	// are unchanged.
	struct SeamedOneTouchDevice : public Devices::OneTouchDevice
	{
		using Devices::OneTouchDevice::OneTouchDevice;   // inherit constructors

		// Force the dead-end fault states so a test can verify actuation is refused and that the
		// device subsequently recovers when comms resume.
		void ForceScrapingFaultedForTest() { m_OpState = OperatingStates::ScrapingFaulted; }
		void ForceFaultHasOccurredForTest() { m_OpState = OperatingStates::FaultHasOccurred; }

		// True once the device has recovered to NormalOperation (the OperatingStates enum is not
		// public, so a test cannot read m_OpState directly).
		bool IsInNormalOperationForTest() const { return m_OpState == OperatingStates::NormalOperation; }

		// Render text onto a screen line exactly as an incoming MessageLong would, so a test can
		// present a recognised page before driving a Status frame.
		void RenderScreenLineForTest(uint8_t line_number, const std::string& text)
		{
			ScreenMode(Devices::Capabilities::ScreenModes::Updating);
			ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evUpdate(line_number, text));
			ProcessScreenUpdates();
		}

		// Drive one Status-message controller update as if a Status frame arrived for our device id.
		void DeliverStatusFrameForTest() { ProcessControllerUpdates(true); }

		// Set the cursor line exactly as an incoming PDAMessage_Highlight would (0xFF = clear-all).
		void SetHighlightedLineForTest(uint8_t line_number) { m_HighlightedLine = line_number; }
	};

}
// namespace AqualinkAutomate::Test
