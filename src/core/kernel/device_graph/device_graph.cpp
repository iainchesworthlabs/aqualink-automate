#include <ranges>

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/device_graph/device_graph.h"
#include "kernel/device_graph/device_graph_filter_by_id.h"
#include "kernel/device_graph/device_graph_filter_by_label.h"

namespace AqualinkAutomate::Kernel
{
	
	DevicesGraph::DevicesGraph()
	{
		m_RootVertexId = boost::add_vertex(std::shared_ptr<AuxillaryDevice>(nullptr), m_DevicesGraph);
	}

	void DevicesGraph::Add(std::shared_ptr<AuxillaryDevice> device)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::Add", std::source_location::current());

		auto insert_device_in_graph = [&](auto& devices_graph, auto& source_vertex, auto& ptr) -> void
		{
			auto target_vertex = boost::add_vertex(ptr, devices_graph);
			auto edge = boost::add_edge(source_vertex, target_vertex, devices_graph);
		};

		if (nullptr == device)
		{
			LogDebug(Channel::Equipment, "DataHub: Failed to add device to device graph -> invalid device provided for insertion");
		}
		else if (Contains(device))
		{
			LogTrace(Channel::Equipment, "DataHub: Did not add device to device graph -> device already exists");
		}
		else
		{
			insert_device_in_graph(m_DevicesGraph, m_RootVertexId, device);
		}
	}

	bool DevicesGraph::Contains(std::shared_ptr<AuxillaryDevice> device) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::Contains", std::source_location::current());

		if (nullptr == device)
		{
			return false;
		}
		else
		{
			auto verts = boost::make_iterator_range(boost::vertices(m_DevicesGraph));
			auto iter = std::ranges::find_if(verts,
				[this, &device](const auto& vertex)
				{
					if (nullptr == m_DevicesGraph[vertex])
					{
						return false;
					}
					else
					{
						// Compare the *objects* not the pointers...
						return (*(m_DevicesGraph[vertex]) == *device);
					}
				}
			);

			return (iter != verts.end());
		}
	}

	void DevicesGraph::Remove(const std::shared_ptr<AuxillaryDevice>& device)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::Remove", std::source_location::current());

		for (auto vp = boost::vertices(m_DevicesGraph); vp.first != vp.second; ++vp.first)
		{
			if (m_DevicesGraph[*vp.first] == device)
			{
				boost::clear_vertex(*vp.first, m_DevicesGraph);
				boost::remove_vertex(*vp.first, m_DevicesGraph);
				return;
			}
		}
	}

	uint32_t DevicesGraph::CountById(const boost::uuids::uuid& id) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::CountById", std::source_location::current());

		DeviceIdFilter filter(m_DevicesGraph, id);
		return CountFilteredView(filter);
	}

	std::shared_ptr<AuxillaryDevice> DevicesGraph::FindById(const boost::uuids::uuid& id) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::FindById", std::source_location::current());

		DeviceIdFilter filter(m_DevicesGraph, id);
		return FindFirstInFilteredView(filter);
	}

	uint32_t DevicesGraph::CountByLabel(std::string_view device_label) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::CountByLabel", std::source_location::current());

		DeviceLabelFilter filter(m_DevicesGraph, device_label);
		return CountFilteredView(filter);
	}

	bool DevicesGraph::HasAnyByLabel(std::string_view device_label) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::HasAnyByLabel", std::source_location::current());

		DeviceLabelFilter filter(m_DevicesGraph, device_label);
		return AnyInFilteredView(filter);
	}

	std::vector<std::shared_ptr<AuxillaryDevice>> DevicesGraph::FindByLabel(std::string_view device_label) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DeviceGraph::FindByLabel", std::source_location::current());

		DeviceLabelFilter filter(m_DevicesGraph, device_label);
		return CollectFilteredView(filter);
	}

}
// namespace AqualinkAutomate::Kernel
