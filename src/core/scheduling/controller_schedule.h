#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace AqualinkAutomate::Scheduling
{

	// Source-level availability of the controller's internal schedule set. The
	// controller's schedules (timers/programs that turn equipment on/off) are not
	// observable from the passive RS-485 stream; they are read by navigating the
	// OneTouch / IAQ "Program" menu pages. Until a capture of those pages has been
	// decoded the parser is a no-op, so the store reports PendingCapture rather
	// than asserting "no schedules exist".
	enum class ControllerScheduleStatus : std::uint8_t
	{
		Available,        // schedules parsed from the controller and listed below
		PendingCapture,   // a capable device is present but the parser is not wired yet
		Unsupported,      // no device that exposes a schedule menu is present
	};

	std::string_view ControllerScheduleStatusToString(ControllerScheduleStatus status);

	// One controller-resident program entry, modelled as a SPAN: equipment
	// `target` is held ON from on-time to off-time on the selected days. This is a
	// deliberately different shape from the app-side Schedule (a point action) so
	// the unified UI can normalise both onto one timeline. The entry is read-only
	// for now; `id` is a stable identifier derived from the source page slot so the
	// UI can key rows without a server-assigned uuid.
	struct ControllerSchedule
	{
		std::string id;
		std::string name;
		std::string target;             // equipment / circuit label the program drives
		std::string group;              // program group this entry belongs to ("A"/"B" or a custom label)
		bool enabled{ true };
		std::uint8_t days_of_week{ 0 }; // bitmask, bit0 = Monday .. bit6 = Sunday
		int on_hour{ 0 };               // 0..23 (local wall-clock)
		int on_minute{ 0 };             // 0..59
		int off_hour{ 0 };
		int off_minute{ 0 };
	};

	// Serialise one span to the wire shape (times as "HH:MM").
	nlohmann::json ToJson(const ControllerSchedule& schedule);

	//=========================================================================
	// ControllerScheduleStore — the latest snapshot of the controller's internal
	// schedules.
	//
	// Populated by the OneTouch/IAQ Program page processors once a capture has
	// been decoded (see PageProcessor_Program). Read by the read-only
	// /api/controller/schedules route. The store always exists so the route can
	// report a status; before any parse it holds PendingCapture and an empty list.
	//
	// Lives on the single cooperative poll() thread — unsynchronised by design.
	//=========================================================================
	class ControllerScheduleStore
	{
	public:
		ControllerScheduleStore() = default;

		ControllerScheduleStatus Status() const { return m_Status; }
		const std::vector<ControllerSchedule>& List() const { return m_Schedules; }

		// The program group the snapshot was read from ("A"/"B" or a custom label).
		// Only the currently-active group's programs are observable on the wire, so
		// this identifies which group the listed schedules belong to (empty until a
		// schedule page has been parsed).
		const std::string& ActiveGroup() const { return m_ActiveGroup; }

		// Replace the snapshot (called by the page processor after a successful
		// parse). Setting Available with the parsed spans, or Unsupported when the
		// present device is known not to expose a schedule menu. active_group names
		// the program group the spans were read from.
		void Replace(ControllerScheduleStatus status, std::vector<ControllerSchedule> schedules, std::string active_group = {})
		{
			m_Status = status;
			m_Schedules = std::move(schedules);
			m_ActiveGroup = std::move(active_group);
		}

	private:
		ControllerScheduleStatus m_Status{ ControllerScheduleStatus::PendingCapture };
		std::vector<ControllerSchedule> m_Schedules;
		std::string m_ActiveGroup;
	};

}
// namespace AqualinkAutomate::Scheduling
