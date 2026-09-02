#pragma once

#include <memory>

#include <nlohmann/json.hpp>

#include "kernel/data_hub.h"

namespace AqualinkAutomate::HTTP::JSON
{

	// One row per addressable aux id (magic_enum::enum_values<JandyAuxillaryIds>()), bounded by
	// AuxillaryModelSpan when the panel model is known (unknown -> every id, same permissive
	// philosophy the span itself documents). Merges live device state with the operator's
	// presence/label overrides so the Settings "Device Names" UI can render both the devices it
	// has found and the slots it hasn't, in one call. Row shape:
	//   { aux_id, power_centre, in_model_span, detected, device_id, label, display_label, status,
	//     presence_override }
	// power_centre is "A"/"B"/"C"/"D", or null for ExtraAux. presence_override is
	// "auto"/"present"/"absent". device_id/label/display_label/status are only present when a
	// live device exists for the slot (detected == true, or a Present override synthesized one).
	//
	// Lives under src/jandy (not src/core/http/json, unlike its siblings) because aux ids and
	// power centres are Jandy-protocol concepts -- libaqualink-automate (core) must never depend
	// on libaqualink-jandy.
	nlohmann::json GenerateJson_Equipment_AuxSlots(const std::shared_ptr<Kernel::DataHub>& data_hub, const nlohmann::json& label_overrides, bool show_aux_id_in_label, const nlohmann::json& presence_overrides);

}
// namespace AqualinkAutomate::HTTP::JSON
