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

}
// namespace AqualinkAutomate::Auxillaries
