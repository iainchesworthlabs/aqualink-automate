#pragma once

#include <string>

#include "auxillaries/jandy_auxillary_id.h"
#include "kernel/auxillary_traits/auxillary_traits_base.h"

namespace AqualinkAutomate::Auxillaries
{

	class JandyAuxillaryId : public Kernel::ImmutableTraitType<const Auxillaries::JandyAuxillaryIds>
	{
	public:
		virtual ~JandyAuxillaryId() = default;
		TraitKey Name() const final { return std::string{"JandyAuxillaryId"}; }
	};

	// Marks a device that ApplyPresenceOverrides synthesized from an operator's "force present"
	// override rather than from real wire evidence. Absent (the common case) means the device
	// was created by one of the ordinary evidence-driven paths (aux status reply, Equipment
	// On/Off page, Label Aux page). Every one of those paths also clears this trait to false the
	// first time it independently touches a device that carries it, so a forced-but-unconfirmed
	// slot's "detected" state flips true the moment the bus actually confirms it -- not merely
	// because a placeholder device object exists in the graph (see GenerateJson_Equipment_AuxSlots).
	class SynthesizedTrait : public Kernel::MutableTraitType<bool>
	{
	public:
		virtual ~SynthesizedTrait() = default;
		TraitKey Name() const final { return std::string{"SynthesizedTrait"}; }
	};

}
// namespace AqualinkAutomate::Auxillaries
