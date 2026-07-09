#pragma once

#include <optional>

#include "kernel/auxillary_traits/auxillary_traits_base.h"
#include "kernel/device_graph/device_graph_types.h"

namespace AqualinkAutomate::Kernel
{

	// IsTraitType is declared in auxillary_traits_base.h (shared with the Traits store).

	template<IsTraitType TRAIT_TYPE>
	class DeviceTraitFilter
	{
	public:
		DeviceTraitFilter(const DevicesGraphType& graph, TRAIT_TYPE trait_type) :
			m_Graph(graph),
			m_TraitType(trait_type),
			m_OptTraitValue(std::nullopt)
		{
		}

		DeviceTraitFilter(const DevicesGraphType& graph, TRAIT_TYPE trait_type, TRAIT_TYPE::TraitValue trait_value) :
			m_Graph(graph),
			m_TraitType(trait_type),
			m_OptTraitValue(trait_value)
		{
		}

		bool operator()(const DevicesGraphType::edge_descriptor) const { return false; }
		bool operator()(const DevicesGraphType::vertex_descriptor vd) const
		{
			auto device = m_Graph[vd];
			if (nullptr == device)
			{
				// Invalid device pointer
				return false;
			}

			// Single trait lookup (TryGet) instead of Has() + operator[] (which previously
			// resolved the trait key twice and built the proxy on the second pass).
			auto trait_value = device->AuxillaryTraits.TryGet(m_TraitType);
			if (!trait_value.has_value())
			{
				// Trait does not exist (or held an incompatible value)
				return false;
			}

			// Didn't need to validate trait value OR trait value matched.
			return !m_OptTraitValue.has_value() || (m_OptTraitValue.value() == trait_value.value());
		}

	private:
		const DevicesGraphType& m_Graph;
		TRAIT_TYPE m_TraitType;
		std::optional<typename TRAIT_TYPE::TraitValue> m_OptTraitValue;
	};

}
// namespace AqualinkAutomate::Kernel
