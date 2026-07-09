#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/graph/filtered_graph.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/uuid/uuid.hpp>

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/device_graph/device_graph_filter_by_trait.h"
#include "kernel/device_graph/device_graph_types.h"
#include "logging/logging.h"
#include "profiling/profiling.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Kernel
{

	class DevicesGraph
	{
	public:
		DevicesGraph();

		void Add(std::shared_ptr<AuxillaryDevice> device);
		bool Contains(std::shared_ptr<AuxillaryDevice> device) const;
		void Remove(const std::shared_ptr<AuxillaryDevice>& device);

		uint32_t CountByLabel(std::string_view device_label) const;
		bool HasAnyByLabel(std::string_view device_label) const;
		std::vector<std::shared_ptr<AuxillaryDevice>> FindByLabel(std::string_view device_label) const;

		uint32_t CountById(const boost::uuids::uuid& id) const;
		std::shared_ptr<AuxillaryDevice> FindById(const boost::uuids::uuid& id) const;

		template<typename TRAIT_TYPE>
		uint32_t CountByTrait(TRAIT_TYPE trait_type) const
		{
			DeviceTraitFilter<TRAIT_TYPE> trait_filter(m_DevicesGraph, trait_type);
			return CountFilteredView(trait_filter);
		}

		template<typename TRAIT_TYPE>
		uint32_t CountByTrait(TRAIT_TYPE trait_type, TRAIT_TYPE::TraitValue trait_value) const
		{
			DeviceTraitFilter<TRAIT_TYPE> trait_filter(m_DevicesGraph, trait_type, trait_value);
			return CountFilteredView(trait_filter);
		}

		template<typename TRAIT_TYPE>
		bool HasAnyByTrait(TRAIT_TYPE trait_type) const
		{
			DeviceTraitFilter<TRAIT_TYPE> trait_filter(m_DevicesGraph, trait_type);
			return AnyInFilteredView(trait_filter);
		}

		template<typename TRAIT_TYPE>
		bool HasAnyByTrait(TRAIT_TYPE trait_type, TRAIT_TYPE::TraitValue trait_value) const
		{
			DeviceTraitFilter<TRAIT_TYPE> trait_filter(m_DevicesGraph, trait_type, trait_value);
			return AnyInFilteredView(trait_filter);
		}

		template<typename TRAIT_TYPE>
		std::vector<std::shared_ptr<AuxillaryDevice>> FindByTrait(TRAIT_TYPE trait_type) const
		{
			DeviceTraitFilter<TRAIT_TYPE> trait_filter(m_DevicesGraph, trait_type);
			return CollectFilteredView(trait_filter);
		}

		template<typename TRAIT_TYPE>
		std::vector<std::shared_ptr<AuxillaryDevice>> FindByTrait(TRAIT_TYPE trait_type, TRAIT_TYPE::TraitValue trait_value) const
		{
			DeviceTraitFilter<TRAIT_TYPE> trait_filter(m_DevicesGraph, trait_type, trait_value);
			return CollectFilteredView(trait_filter);
		}

	private:
		template<typename Filter>
		auto MakeFilteredView(const Filter& filter) const
		{
			return boost::filtered_graph<DevicesGraphType, boost::keep_all, Filter>(m_DevicesGraph, boost::keep_all{}, filter);
		}

		// Counts the vertices that survive the supplied vertex filter. The
		// filtered view excludes null device vertices (every filter rejects a
		// null pointee), so no extra null guard is required here.
		template<typename Filter>
		uint32_t CountFilteredView(const Filter& filter) const
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::CountFilteredView", std::source_location::current());

			auto fg = MakeFilteredView(filter);
			auto range = boost::make_iterator_range(boost::vertices(fg));
			return static_cast<uint32_t>(std::distance(range.begin(), range.end()));
		}

		// Returns true as soon as a single vertex survives the filter, without
		// materialising a vector or counting the whole view.
		template<typename Filter>
		bool AnyInFilteredView(const Filter& filter) const
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::AnyInFilteredView", std::source_location::current());

			auto fg = MakeFilteredView(filter);
			auto [begin, end] = boost::vertices(fg);
			return (begin != end);
		}

		// Collects the surviving device pointers into a vector, applying the
		// null-vertex check uniformly as a defensive guard.
		template<typename Filter>
		std::vector<std::shared_ptr<AuxillaryDevice>> CollectFilteredView(const Filter& filter) const
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::CollectFilteredView", std::source_location::current());

			std::vector<std::shared_ptr<AuxillaryDevice>> found_devices;

			auto fg = MakeFilteredView(filter);

			for (auto vp : boost::make_iterator_range(boost::vertices(fg)))
			{
				if (auto device_ptr = m_DevicesGraph[vp]; nullptr != device_ptr)
				{
					found_devices.push_back(std::move(device_ptr));
				}
			}

			return found_devices;
		}

		// Returns the first surviving device pointer, or nullptr when the
		// filtered view is empty (applies the null-vertex check uniformly).
		template<typename Filter>
		std::shared_ptr<AuxillaryDevice> FindFirstInFilteredView(const Filter& filter) const
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::FindFirstInFilteredView", std::source_location::current());

			auto fg = MakeFilteredView(filter);

			for (auto vp : boost::make_iterator_range(boost::vertices(fg)))
			{
				if (auto device_ptr = m_DevicesGraph[vp]; nullptr != device_ptr)
				{
					return device_ptr;
				}
			}

			return nullptr;
		}

		// NOTE: The device graph is intentionally unsynchronised. Device
		// registration (from the protocol task) and queries (from the
		// HTTP/diagnostics handlers) both run on the single application thread
		// driven by the main poll() loop, so no locking is required. If a
		// multi-threaded execution model is ever reintroduced, this container
		// must be guarded before concurrent iteration/mutation.

	private:
		DevicesGraphType m_DevicesGraph;
		DeviceVertexType m_RootVertexId;
	};
	

}
// namespace AqualinkAutomate::Kernel
