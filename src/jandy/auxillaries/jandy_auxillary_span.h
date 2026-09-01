#pragma once

#include <cstddef>
#include <cstdint>

#include "auxillaries/jandy_auxillary_id.h"

namespace AqualinkAutomate::Kernel
{
	class DataHub;
	class DevicesGraph;
}
// namespace AqualinkAutomate::Kernel

namespace AqualinkAutomate::Auxillaries
{

	// The set of auxillary relays the PANEL'S OWN decoded model can physically have.
	//
	// This exists because a status QUERY is not evidence of existence: the emulated Serial
	// Adapter (RSSA) round-robins a status query over its whole command vocabulary, and a
	// reply about an aux the panel does not have would otherwise be turned into a device
	// (labelled with the generic enum name -- "Aux B1", "Aux D6"). Bounding the sweep by the
	// model the panel itself reports keeps discovery evidence-driven and stays correct for
	// every install: nothing here is hardcoded to one site's equipment, the numbers come from
	// PoolConfigurationDecoder via the version/REV page.
	//
	// The span is UNKNOWN until that page has been scraped (SystemBoard still Unknown, or a
	// zero power-centre count) and an unknown span contains everything, so a not-yet-identified
	// panel is never trimmed. It bounds the NUMBERED relays only: ExtraAux belongs to no power
	// centre and is not counted by the model tables, so the span has no opinion about it and
	// always admits it.
	class AuxillaryModelSpan
	{
	public:
		AuxillaryModelSpan() = default;
		AuxillaryModelSpan(uint8_t power_centers, uint8_t auxillary_count);

		// Build from the model facts the version/REV scrape wrote into the DataHub. Returns an
		// unknown (permissive) span while the panel model has not been identified.
		static AuxillaryModelSpan FromDataHub(const Kernel::DataHub& data_hub);

		bool IsKnown() const;

		// Can this aux id exist on the decoded model? Always true while the span is unknown.
		bool Contains(JandyAuxillaryIds id) const;

	private:
		uint8_t m_PowerCenters{ 0 };     // 0 == unknown
		uint8_t m_AuxillaryCount{ 0 };   // 0 == unknown; a TOTAL across centres, not per-centre
	};

	// Remove every auxillary device whose aux id lies outside `span`, returning how many were
	// removed. Devices whose aux id cannot be resolved (from the JandyAuxillaryId trait, the
	// hardware label, or a generic label) are never touched -- only a device that positively
	// identifies as an aux the model cannot have is pruned. A no-op while the span is unknown.
	//
	// An out-of-span aux carrying an OPERATOR-ASSIGNED label is also kept: being named is
	// evidence that something which enumerates real equipment saw it, and that outranks a model
	// table that may be wrong for this panel. Those are reported by the equipment validator as
	// anomalies instead of being deleted.
	//
	// This also cleans a persisted equipment cache: phantoms restored from an earlier run are
	// pruned at the first version-page scrape rather than surviving forever.
	std::size_t PruneAuxillariesOutsideSpan(Kernel::DevicesGraph& devices, const AuxillaryModelSpan& span);

}
// namespace AqualinkAutomate::Auxillaries
