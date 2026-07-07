#pragma once

#include <any>
#include <cstdint>
#include <initializer_list>
#include <unordered_map>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graph_utility.hpp>

#include "utility/screen_data_page_graph/screen_data_page_graph_edge.h"
#include "utility/screen_data_page_graph/screen_data_page_graph_vertex.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Utility
{

	namespace ScreenDataPageGraphImpl
	{
		// Forward declarations.
		class ForwardIterator;
	}
	// namespace ScreenDataPageGraphImpl

	class ScreenDataPageGraph
	{
		friend class ScreenDataPageGraphImpl::ForwardIterator;

	public:
		using GraphType = boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS, ScreenDataPageGraphImpl::Vertex, ScreenDataPageGraphImpl::Edge>;
		using VertexType = boost::graph_traits<GraphType>::vertex_descriptor;
		using EdgeType = boost::graph_traits<GraphType>::edge_descriptor;

	public:
		ScreenDataPageGraph()
		{
		}

		ScreenDataPageGraph(std::initializer_list<ScreenDataPageGraphImpl::Vertex> vertices, std::initializer_list<std::tuple<ScreenDataPageGraphImpl::VertexId, ScreenDataPageGraphImpl::VertexId, ScreenDataPageGraphImpl::Edge>> edges)
		{
			for (auto& vertex : vertices)
			{
				AddVertex(vertex);
			}

			for (auto& [source, destination, edge] : edges)
			{
				AddEdge(source, destination, edge);
			}
		}

		void AddVertex(ScreenDataPageGraphImpl::Vertex new_vertex)
		{
			m_VertexMap[new_vertex.id] = boost::add_vertex(new_vertex, m_Graph);
		}

		void AddEdge(ScreenDataPageGraphImpl::VertexId source_id, ScreenDataPageGraphImpl::VertexId destination_id, ScreenDataPageGraphImpl::Edge new_edge)
		{
			boost::add_edge(m_VertexMap[source_id], m_VertexMap[destination_id], new_edge, m_Graph);
		}

	private:
		std::unordered_map<uint32_t, VertexType> m_VertexMap;
		GraphType m_Graph;
	};

}
// namespace AqualinkAutomate::Utility
