#pragma once

#include <string>
#include <string_view>

#include "kernel/device_graph/device_graph_types.h"

namespace AqualinkAutomate::Kernel
{

	class DeviceLabelFilter
	{
	public:
		DeviceLabelFilter(const DevicesGraphType& graph, std::string_view device_label);

		bool operator()(const DevicesGraphType::edge_descriptor) const;
		bool operator()(const DevicesGraphType::vertex_descriptor vd) const;

	private:
		const DevicesGraphType& m_Graph;
		std::string m_DeviceLabel;
	};

}
// namespace AqualinkAutomate::Kernel
