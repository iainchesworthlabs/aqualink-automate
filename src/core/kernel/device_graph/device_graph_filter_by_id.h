#pragma once

#include <boost/uuid/uuid.hpp>

#include "kernel/device_graph/device_graph_types.h"

namespace AqualinkAutomate::Kernel
{

	class DeviceIdFilter
	{
	public:
		DeviceIdFilter(const DevicesGraphType& graph, const boost::uuids::uuid& device_id);

		bool operator()(const DevicesGraphType::edge_descriptor) const;
		bool operator()(const DevicesGraphType::vertex_descriptor vd) const;

	private:
		const DevicesGraphType& m_Graph;
		// Stored by value: a filter may outlive the caller-supplied uuid (e.g. a temporary),
		// so holding a reference here was a dangling-reference hazard.
		const boost::uuids::uuid m_DeviceId;
	};

}
// namespace AqualinkAutomate::Kernel
