#include "navigation/menu_model.h"

#include <algorithm>
#include <queue>
#include <source_location>
#include <utility>

#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Navigation
{

	namespace
	{
		// Reconstruct a path by walking predecessor edges back from the edge that first
		// reached the destination to the BFS start, then reversing into forward order.
		std::vector<const MenuEdge*> ReconstructPath(
			const MenuEdge* reaching_edge,
			const std::unordered_map<PageId, const MenuEdge*>& predecessor,
			PageId from,
			PageId to)
		{
			std::vector<const MenuEdge*> path;
			for (const MenuEdge* e = reaching_edge; e != nullptr; e = predecessor.at(e->source))
			{
				path.push_back(e);
			}
			std::ranges::reverse(path);

			LogDebug(Channel::Navigation, [&from, &to, &path] { return std::format("MenuModel: Found path from {} to {} with {} steps",
				std::to_underlying(from), std::to_underlying(to), path.size()); });
			return path;
		}
	}
	// anonymous namespace

	// =========================================================================
	// MenuPage convenience methods
	// =========================================================================

	std::optional<PageId> MenuPage::BackTarget() const
	{
		for (const auto& edge : edges)
		{
			if (edge.trigger == EdgeTrigger::Back)
			{
				return edge.target;
			}
		}
		return std::nullopt;
	}

	bool MenuPage::SupportsKey(EdgeTrigger trigger) const
	{
		for (const auto& edge : edges)
		{
			if (edge.trigger == trigger)
			{
				return true;
			}
		}
		return false;
	}

	// =========================================================================
	// MenuModel
	// =========================================================================

	void MenuModel::RegisterPage(MenuPage page)
	{
		LogTrace(Channel::Navigation, [&page] { return std::format("MenuModel: Registering page '{}' (id={})",
			page.name, std::to_underlying(page.id)); });
		m_Pages[page.id] = std::move(page);

		// A new page may introduce new incoming-Select edges; invalidate the memoised index.
		m_IncomingSelectEdgesValid = false;
	}

	void MenuModel::RegisterGlobalEdge(MenuEdge edge)
	{
		LogTrace(Channel::Navigation, [&edge] { return std::format("MenuModel: Registering global edge '{}' -> page {}",
			edge.label, std::to_underlying(edge.target)); });
		m_GlobalEdges.push_back(std::move(edge));
	}

	const MenuPage* MenuModel::GetPage(PageId id) const
	{
		auto it = m_Pages.find(id);
		if (it == m_Pages.end())
		{
			return nullptr;
		}
		return &it->second;
	}

	const MenuPage* MenuModel::FindPageByDetection(const Utility::ScreenDataPage& content) const
	{
		PageId detected = DetectPage(content);
		if (detected == PageId::Unknown)
		{
			return nullptr;
		}
		return GetPage(detected);
	}

	PageId MenuModel::DetectPage(const Utility::ScreenDataPage& content) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MenuModel::DetectPage", std::source_location::current());

		// Try to match each registered page's detection patterns
		// All detectors for a page must match for it to be identified
		PageId best_match = PageId::Unknown;
		size_t best_detector_count = 0;
		size_t best_pattern_length = 0;

		for (const auto& [id, page] : m_Pages)
		{
			if (page.detectors.empty())
			{
				continue;
			}

			bool all_match = std::ranges::all_of(page.detectors,
				[&content](const auto& detector)
				{
					return detector.line < content.Size() &&
						content[detector.line].Text.find(detector.pattern) != std::string::npos;
				});

			if (all_match)
			{
				// Check max_content_lines constraint if set
				if (page.max_content_lines.has_value())
				{
					uint8_t non_empty_count = 0;
					for (size_t i = 0; i < content.Size(); ++i)
					{
						if (!content[i].Text.empty())
						{
							non_empty_count++;
						}
					}
					if (non_empty_count > page.max_content_lines.value())
					{
						continue;  // Reject this page match
					}
				}

				// Calculate total pattern length for specificity tiebreaking
				size_t total_pattern_length = 0;
				for (const auto& d : page.detectors)
				{
					total_pattern_length += d.pattern.length();
				}

				LogTrace(Channel::Navigation, [&page, total_pattern_length] { return std::format("MenuModel: Page '{}' matched with {} detectors (pattern_len={})",
					page.name, page.detectors.size(), total_pattern_length); });

				// Prefer matches with more detectors, then longer patterns as tiebreaker
				if (page.detectors.size() > best_detector_count ||
					(page.detectors.size() == best_detector_count && total_pattern_length > best_pattern_length))
				{
					best_match = id;
					best_detector_count = page.detectors.size();
					best_pattern_length = total_pattern_length;
				}
			}
		}

		if (best_match != PageId::Unknown)
		{
			const MenuPage* matched_page = GetPage(best_match);
			LogTrace(Channel::Navigation, [&matched_page, &best_match, &best_detector_count] { return std::format("MenuModel: Best match -> '{}' (id={}) with {} detectors",
				matched_page->name, std::to_underlying(best_match), best_detector_count); });

			// Log the detector patterns that matched
			for (const auto& detector : matched_page->detectors)
			{
				if (detector.line < content.Size())
				{
					LogTrace(Channel::Navigation, [&detector, &content] { return std::format("  Detector: line {} pattern '{}' matched in '{}'",
						detector.line, detector.pattern, content[detector.line].Text); });
				}
			}
		}
		else
		{
			LogTrace(Channel::Navigation, "MenuModel: No page detected - dumping screen content:");
			for (size_t i = 0; i < content.Size() && i < 12; ++i)
			{
				if (!content[i].Text.empty())
				{
					LogTrace(Channel::Navigation, [&i, &content] { return std::format("  Line {}: '{}'", i, content[i].Text); });
				}
			}
		}

		return best_match;
	}

	PageId MenuModel::FindPageIdByType(Utility::ScreenDataPageTypes page_type) const
	{
		// Find the PageId that corresponds to the given ScreenDataPageTypes
		for (const auto& [id, page] : m_Pages)
		{
			if (page.page_type == page_type)
			{
				return id;
			}
		}
		return PageId::Unknown;
	}

	std::vector<const MenuEdge*> MenuModel::FindPath(PageId from, PageId to) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MenuModel::FindPath", std::source_location::current());

		if (from == to)
		{
			return {}; // Already at destination
		}

		// BFS to find the shortest path using edges. Rather than carrying (and deep-copying)
		// the accumulated path on each frontier node, record the edge that first reached each
		// page in a predecessor map and reconstruct the path once on success. This keeps the
		// BFS allocation-light: one map entry per visited page instead of an O(depth) vector
		// copy per expanded edge.
		std::queue<PageId> queue;
		std::unordered_map<PageId, const MenuEdge*> predecessor; // page -> edge that reached it

		queue.push(from);
		predecessor[from] = nullptr; // sentinel marks the start (and "visited")

		while (!queue.empty())
		{
			const PageId current = queue.front();
			queue.pop();

			const MenuPage* page = GetPage(current);
			if (!page)
			{
				continue;
			}

			// Iterate all edges from this page, considering only page transitions
			for (const auto& edge : page->edges)
			{
				if (!edge.IsPageTransition())
				{
					continue; // Skip self-loops (LineUp, LineDown, PageUp, PageDown)
				}

				if (predecessor.contains(edge.target))
				{
					continue; // Already visited
				}

				predecessor[edge.target] = &edge;

				if (edge.target == to)
				{
					// Reconstruct the path by walking predecessors back to the start.
					return ReconstructPath(&edge, predecessor, from, to);
				}

				queue.push(edge.target);
			}
		}

		LogTrace(Channel::Navigation, [&from, &to] { return std::format("MenuModel: No path found from {} to {}",
			std::to_underlying(from), std::to_underlying(to)); });
		return {}; // No path found
	}

	void MenuModel::RebuildIncomingSelectEdges() const
	{
		m_IncomingSelectEdges.clear();

		for (const auto& [id, page] : m_Pages)
		{
			for (const auto& edge : page.edges)
			{
				if (edge.trigger == EdgeTrigger::Select && edge.IsPageTransition())
				{
					m_IncomingSelectEdges[edge.target].push_back(&edge);
				}
			}
		}

		m_IncomingSelectEdgesValid = true;
	}

	std::vector<const MenuEdge*> MenuModel::GetIncomingSelectEdges(PageId target) const
	{
		// The index is built once over the whole page set and reused across crawl steps,
		// rather than rescanning every page+edge (and allocating a fresh vector) per call.
		if (!m_IncomingSelectEdgesValid)
		{
			RebuildIncomingSelectEdges();
		}

		const auto it = m_IncomingSelectEdges.find(target);
		if (it == m_IncomingSelectEdges.end())
		{
			return {};
		}

		return it->second;
	}

	std::optional<MenuEdge> MenuModel::FindSystemEvent(PageId detected_page) const
	{
		for (const auto& edge : m_GlobalEdges)
		{
			if (edge.target == detected_page)
			{
				return edge;
			}
		}
		return std::nullopt;
	}

}
// namespace AqualinkAutomate::Navigation
