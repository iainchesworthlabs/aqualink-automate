#pragma once

#include <tuple>
#include <unordered_set>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graph_utility.hpp>

#include "utility/screen_data_page_graph.h"
#include "utility/screen_data_page_graph/screen_data_page_graph_edge.h"
#include "utility/screen_data_page_graph/screen_data_page_graph_vertex.h"

namespace AqualinkAutomate::Utility::ScreenDataPageGraphImpl
{

    class ForwardIterator
    {
    public:
        using value_type = std::tuple<Vertex, Edge>;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type const *;
        using reference = value_type const &;
        using iterator_category = std::forward_iterator_tag;

    public:
        ForwardIterator(ScreenDataPageGraph& graph, VertexId start_id);
        ForwardIterator(const ForwardIterator& other);
        ForwardIterator(ForwardIterator&& other) noexcept;

        ForwardIterator& operator=(const ForwardIterator& other) = delete;
        ForwardIterator& operator=(ForwardIterator&& other) noexcept = delete;

        ForwardIterator& operator++();
        ForwardIterator operator++(int);

        ForwardIterator::reference operator*() const;
        ForwardIterator::pointer operator->() const;

        bool operator==(const ForwardIterator& other) const;
        bool operator!=(const ForwardIterator& other) const;

        static ForwardIterator begin(ScreenDataPageGraph& graph, VertexId start_id);
        static ForwardIterator end(ScreenDataPageGraph& graph);

    private:
        ScreenDataPageGraph& m_Graph;
        value_type m_CurrentStep;
        std::unordered_set<VertexId> m_Visited;
    };

}
// using namespace AqualinkAutomate::Utility::ScreenDataPageGraphImpl
