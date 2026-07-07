#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "devices/jandy_device_types.h"
#include "formatters/jandy_device_formatters.h"  // std::formatter<JandyDeviceType> for the "OneTouch ({})", DeviceId() logs
#include "kernel/data_hub.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "scheduling/controller_schedule.h"
#include "utility/screen_data_page.h"
#include "utility/screen_data_page_processor.h"

namespace AqualinkAutomate::Devices
{

	// Read path of the OneTouch controller: the page + status processors that turn a
	// reconstructed menu screen into DataHub state, split out of OneTouchDevice so the
	// (screen -> DataHub) transforms live in one focused, separately-testable collaborator.
	//
	// OneTouchDevice owns one instance and registers its processors (via MakeProcessors) into
	// the Screen capability; each processor closure is bound to this object. The scraper also
	// owns the controller-schedule accumulation, because the OneTouch shows ONE Program per
	// per-equipment detail page and the snapshot is folded together across page visits.
	//
	// It borrows (shared_ptr) the same DataHub and ControllerScheduleStore the device resolved
	// from the HubLocator, and a handle to the device id purely for log context.
	class OneTouchScraper
	{
	public:
		// Set AquaPure page layout (verified vs onetouch_chlorinator.cap): "Set Pool to:" is on
		// line 3, "Set Spa to:" on line 4. Public because the value editor (OneTouchDevice) drives
		// the same Pool row when setting the chlorinator %.
		inline static constexpr uint8_t SETAQUAPURE_POOL_LINE{ 3 };
		inline static constexpr uint8_t SETAQUAPURE_SPA_LINE{ 4 };

		OneTouchScraper(
			std::shared_ptr<Devices::JandyDeviceType> device_id,
			std::shared_ptr<Kernel::DataHub> data_hub,
			std::shared_ptr<Scheduling::ControllerScheduleStore> schedule_store);

		// The full set of page processors bound to this scraper, ready to hand to
		// Screen::PageProcessors() from the OneTouchDevice constructor.
		std::list<Utility::ScreenDataPage_Processor> MakeProcessors();

	private:
		// Device id for log context. Mirrors OneTouchDevice::DeviceId() so the moved processor
		// bodies (which log "OneTouch ({})", DeviceId()) read unchanged.
		const Devices::JandyDeviceType& DeviceId() const { return *m_DeviceId; }

		// Page processors (screen -> DataHub). One per navigable page; several are log-only stubs
		// kept so the page stays registered and therefore detectable/navigable.
		void PageProcessor_System(const Utility::ScreenDataPage& page);
		void PageProcessor_Service(const Utility::ScreenDataPage& page);
		void PageProcessor_TimeOut(const Utility::ScreenDataPage& page);
		void PageProcessor_OneTouch(const Utility::ScreenDataPage& page);
		void PageProcessor_EquipmentOnOff(const Utility::ScreenDataPage& page);
		void PageProcessor_EquipmentStatus(const Utility::ScreenDataPage& page);
		void PageProcessor_SelectSpeed(const Utility::ScreenDataPage& page);
		void PageProcessor_MenuHelp(const Utility::ScreenDataPage& page);
		void PageProcessor_HelpSubmenu(const Utility::ScreenDataPage& page);
		void PageProcessor_SetTemperature(const Utility::ScreenDataPage& page);
		void PageProcessor_SetTime(const Utility::ScreenDataPage& page);
		void PageProcessor_SystemSetup(const Utility::ScreenDataPage& page);
		void PageProcessor_FreezeProtect(const Utility::ScreenDataPage& page);
		void PageProcessor_Boost(const Utility::ScreenDataPage& page);
		void PageProcessor_SetAquapure(const Utility::ScreenDataPage& page);
		void PageProcessor_Version(const Utility::ScreenDataPage& page);
		void PageProcessor_DiagnosticsSensors(const Utility::ScreenDataPage& page);
		void PageProcessor_DiagnosticsRemotes(const Utility::ScreenDataPage& page);
		void PageProcessor_DiagnosticsErrors(const Utility::ScreenDataPage& page);
		void PageProcessor_LabelAuxList(const Utility::ScreenDataPage& page);
		void PageProcessor_LabelAux(const Utility::ScreenDataPage& page);
		void PageProcessor_MoreOneTouch(const Utility::ScreenDataPage& page);
		void PageProcessor_SetPoolHeat(const Utility::ScreenDataPage& page);
		void PageProcessor_SetSpaHeat(const Utility::ScreenDataPage& page);
		void PageProcessor_Program(const Utility::ScreenDataPage& page);
		void PageProcessor_DisplayLight(const Utility::ScreenDataPage& page);
		void PageProcessor_Lockouts(const Utility::ScreenDataPage& page);
		void PageProcessor_PasswordSettings(const Utility::ScreenDataPage& page);
		void PageProcessor_ProgramGroup(const Utility::ScreenDataPage& page);
		void PageProcessor_GeneralLabels(const Utility::ScreenDataPage& page);
		void PageProcessor_LightLabels(const Utility::ScreenDataPage& page);
		void PageProcessor_WaterfallLabels(const Utility::ScreenDataPage& page);
		void PageProcessor_CustomLabel(const Utility::ScreenDataPage& page);
		void PageProcessor_EnterPassword(const Utility::ScreenDataPage& page);
		void PageProcessor_HelpKeys(const Utility::ScreenDataPage& page);
		void PageProcessor_SpaSwitch(const Utility::ScreenDataPage& page);
		void PageProcessor_StartUp(const Utility::ScreenDataPage& page);

		static constexpr uint32_t HINT_COUNT{ 2 };
		using HintArrayType = std::array<unsigned char, HINT_COUNT>;

		bool StatusProcessor_ShouldSkipLineProcessing(const HintArrayType& hint_array, const std::string_view line_to_process) const;
		void StatusProcessor_FilterPump(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_PoolHeat(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_SpaHeat(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_SolarHeat(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_HeatPump(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_Chiller(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_AquaPurePercentage(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_SaltLevelPPM(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_CheckAquaPure(const Utility::ScreenDataPage& page, const uint8_t line_id);

		// Shared decode for the two pages carrying the panel identity + pool configuration (the
		// cold-start splash and the REV page); builds the bodies of water via
		// DataHub::ApplyPoolConfiguration so both call sites stay consistent.
		void DecodePanelConfiguration(const Utility::ScreenDataPage& page);

		// Parse a just-completed Program detail page, fold it into m_ControllerSchedules, and
		// republish the accumulated snapshot to m_ControllerScheduleStore (status Available).
		void PublishControllerSchedules(const Utility::ScreenDataPage& page);

		// On the Equipment On/Off page: if new_device carries a stable aux id AND a device with
		// that id already exists in the graph, reconcile onto the existing device in place
		// (grant identity, copy status, prune legacy placeholders) and return true so the caller
		// skips adding a duplicate. Returns false when there is no existing device to reconcile.
		bool ReconcileExistingEquipmentAux(const std::shared_ptr<Kernel::AuxillaryDevice>& new_device);

	private:
		std::shared_ptr<Devices::JandyDeviceType> m_DeviceId;
		std::shared_ptr<Kernel::DataHub> m_DataHub;

		// Read-only sink for the controller's internal schedules parsed off the per-equipment
		// Program detail pages (the /api/controller/schedules source). Null on a passive/test rig.
		std::shared_ptr<Scheduling::ControllerScheduleStore> m_ControllerScheduleStore;

		// Accumulator keyed by (target, program-index): the OneTouch shows one program per detail
		// page, so each visit adds/updates its entry and the whole snapshot is republished.
		std::map<std::pair<std::string, int>, Scheduling::ControllerSchedule> m_ControllerSchedules;

		// The active Program Group (empty until read off the Program Group page); stamped onto each
		// schedule passed to the store.
		std::string m_ControllerScheduleGroup;
	};

}
// namespace AqualinkAutomate::Devices
