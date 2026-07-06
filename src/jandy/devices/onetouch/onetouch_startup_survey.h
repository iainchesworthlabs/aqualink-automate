#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AqualinkAutomate::Kernel { class DataHub; }
namespace AqualinkAutomate::Devices { class JandyDeviceType; }
namespace AqualinkAutomate::Navigation { class SpiderEngine; class MenuModel; }

namespace AqualinkAutomate::Devices::OneTouch
{

	// Health summary of the startup menu crawl: which pages were reached, and which the crawl could
	// not reach -- split into capability-gated pages whose absence is EXPECTED on this model (e.g.
	// the iAqualink or chlorinator pages) versus NOTABLE failures every panel should have. Surfaced
	// via OneTouchDevice::DescribeDiagnostics.
	struct MenuSurveyResult
	{
		uint32_t PagesReached{ 0 };
		std::vector<std::string> ExpectedAbsent;    // failed but capability-gated (benign)
		std::vector<std::string> NotableFailures;   // failed and expected on every model
		bool EquipmentPageReached{ false };          // the critical Equipment ON/OFF page
	};

	// Startup-crawl analysis helpers, extracted from OneTouchDevice as pure functions of their
	// inputs (the op-state machine that calls them stays on the device):

	// True when the DataHub already carries one or more aux devices with a non-empty label (e.g.
	// seeded passively by a real iAqualink2 on the bus). When so, the emulated OneTouch skips the
	// slow "Label Aux" menu crawl at startup and reuses those labels.
	bool DataHubHasSeededAuxLabels(const Kernel::DataHub& data_hub);

	// Cross-check the discovered equipment set against the model's expected aux/power-centre layout
	// and record the outcome on the DataHub (EquipmentValidationResult); logs any anomalies.
	void ValidateDiscoveredEquipment(Kernel::DataHub& data_hub, const Devices::JandyDeviceType& device_id);

	// Summarise a completed menu crawl: reached/failed pages, classifying failures as expected-absent
	// (capability-gated) vs notable, and warn if a core page (Equipment ON/OFF) was missed.
	MenuSurveyResult BuildMenuSurvey(const Navigation::SpiderEngine& engine, const Navigation::MenuModel& model, const Devices::JandyDeviceType& device_id);

}
// namespace AqualinkAutomate::Devices::OneTouch
