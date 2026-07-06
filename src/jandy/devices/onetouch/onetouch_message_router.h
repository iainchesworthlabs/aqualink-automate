#pragma once

#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_message_long.h"
#include "messages/jandy_message_status.h"
#include "messages/jandy_message_display_update.h"
#include "messages/jandy_message_unknown.h"
#include "messages/pda/pda_message_clear.h"
#include "messages/pda/pda_message_highlight.h"
#include "messages/pda/pda_message_highlight_chars.h"
#include "messages/pda/pda_message_shiftlines.h"

namespace AqualinkAutomate::Devices
{

	class OneTouchDevice;

	// Wire-message ingest for the OneTouch controller: the boost::signals2 slot handlers that turn
	// each RS-485 frame (addressed to our device id) into a screen update + a controller tick +
	// a watchdog kick. Split out of OneTouchDevice so the message-routing responsibility is a
	// distinct unit from the device's capabilities / op-state / lifecycle.
	//
	// The handlers are intrinsically coupled to the device's Screen capability, its op-state tick
	// (ProcessControllerUpdates) and its watchdog, so the router holds a reference to the owning
	// device and is a friend of it. The device registers these handlers (bound to the router) in
	// its constructor.
	class OneTouchMessageRouter
	{
	public:
		explicit OneTouchMessageRouter(OneTouchDevice& device) : m_Device(device) {}

		void Slot_OneTouch_Ack(const Messages::JandyMessage_Ack& msg);
		void Slot_OneTouch_MessageLong(const Messages::JandyMessage_MessageLong& msg);
		void Slot_OneTouch_Probe(const Messages::JandyMessage_Probe& msg);
		void Slot_OneTouch_Status(const Messages::JandyMessage_Status& msg);
		void Slot_OneTouch_Clear(const Messages::PDAMessage_Clear& msg);
		void Slot_OneTouch_Highlight(const Messages::PDAMessage_Highlight& msg);
		void Slot_OneTouch_HighlightChars(const Messages::PDAMessage_HighlightChars& msg);
		void Slot_OneTouch_ShiftLines(const Messages::PDAMessage_ShiftLines& msg);
		void Slot_OneTouch_DisplayUpdate(const Messages::JandyMessage_DisplayUpdate& msg);
		void Slot_OneTouch_Unknown(const Messages::JandyMessage_Unknown& msg);

	private:
		OneTouchDevice& m_Device;
	};

}
// namespace AqualinkAutomate::Devices
