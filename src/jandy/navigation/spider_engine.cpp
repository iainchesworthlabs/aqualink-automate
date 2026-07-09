#include "navigation/spider_engine.h"

#include <source_location>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Navigation
{

	SpiderEngine::SpiderEngine(const MenuModel& model, Navigator& navigator)
		: m_Model(model)
		, m_Navigator(navigator)
	{
	}

	void SpiderEngine::StartCrawl(std::unique_ptr<VisitPolicy> policy)
	{
		m_Policy = std::move(policy);
		m_Visited.clear();
		m_Failed.clear();
		m_VisitedMultiEdges.clear();
		m_CurrentMultiEdge = std::nullopt;
		m_CurrentTarget = PageId::Unknown;
		m_NavigationFailures = 0;

		// If Navigator is not yet synced, start syncing first
		if (!m_Navigator.IsSynced())
		{
			LogInfo(Channel::Scraping, "SpiderEngine: Starting crawl - Navigator not synced, syncing first");
			m_Navigator.StartSync();
			m_State = State::Syncing;
		}
		else
		{
			LogInfo(Channel::Scraping, std::format("SpiderEngine: Starting crawl from page {}",
				std::to_underlying(m_Navigator.GetCurrentPage())));
			m_State = State::NavigatingToNext;
			NavigateToNextTarget();
		}
	}

	std::optional<NavKeyCommand> SpiderEngine::ProcessStep(
		const Utility::ScreenDataPage& content,
		uint8_t highlighted_line)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SpiderEngine::ProcessStep", std::source_location::current());

		switch (m_State)
		{
		case State::Idle:
		case State::Complete:
		case State::Failed:
			return std::nullopt;

		case State::Syncing:
		{
			// Feed page updates to Navigator for sync
			auto cmd = m_Navigator.OnPageUpdate(content, highlighted_line);

			if (m_Navigator.IsSynced())
			{
				LogInfo(Channel::Scraping, std::format("SpiderEngine: Navigator synced to page {} - beginning crawl",
					std::to_underlying(m_Navigator.GetCurrentPage())));
				m_State = State::NavigatingToNext;
				NavigateToNextTarget();

				// If we already have a target, start navigating
				if (m_State == State::NavigatingToNext)
				{
					return m_Navigator.OnPageUpdate(content, highlighted_line);
				}
			}

			return cmd;
		}

		case State::NavigatingToNext:
		{
			// Drive the Navigator
			auto cmd = m_Navigator.OnPageUpdate(content, highlighted_line);

			if (m_Navigator.IsComplete())
			{
				if (m_Navigator.IsSuccess())
				{
					// Arrived at target page
					LogInfo(Channel::Scraping, std::format("SpiderEngine: Arrived at target page {}",
						std::to_underlying(m_CurrentTarget)));
					m_State = State::CapturingPage;
					m_NavigationFailures = 0;

					// Capture immediately
					return ProcessStep(content, highlighted_line);
				}
				else
				{
					// Navigation failed.
					if (m_CurrentMultiEdge.has_value())
					{
						// A failed multi-instance edge is a BENIGN, EXPECTED outcome:
						// it means that particular instance does not exist on this
						// controller (e.g. an "Aux B1" row when no power center B is
						// installed - the Navigator scrolled the list and never found
						// it). This must NOT count toward the crawl-wide failure budget,
						// otherwise a controller with only power center A would abort the
						// whole crawl after the first few absent B/C/D auxes. Mark just
						// this edge visited and move on to the next aux.
						auto edge = m_CurrentMultiEdge.value();
						LogDebug(Channel::Scraping, std::format("SpiderEngine: Multi-instance edge (source={}, line={} '{}') not reachable - skipping (instance absent on this controller)",
							std::to_underlying(edge->source), edge->trigger_line, edge->label));
						m_VisitedMultiEdges[m_CurrentTarget].insert({ edge->source, edge->trigger_line });
						m_CurrentMultiEdge = std::nullopt;

						// If this was the last incoming edge for the page, the page is
						// now fully visited even though this final instance was skipped.
						// (The success path marks the page visited in CapturingPage; the
						// skip path must do the same so the page isn't left perpetually
						// "unvisited" when its last instance is absent.)
						if (!HasUnvisitedMultiEdges(m_CurrentTarget))
						{
							m_Visited.insert(m_CurrentTarget);
							LogInfo(Channel::Scraping, std::format("SpiderEngine: All instances of multi-instance page {} processed (last one absent)",
								std::to_underlying(m_CurrentTarget)));
						}
					}
					else
					{
						m_NavigationFailures++;
						LogWarning(Channel::Scraping, std::format("SpiderEngine: Navigation to page {} failed ({}/{})",
							std::to_underlying(m_CurrentTarget), m_NavigationFailures, MAX_NAVIGATION_FAILURES));

						if (m_NavigationFailures >= MAX_NAVIGATION_FAILURES)
						{
							LogError(Channel::Scraping, "SpiderEngine: Max navigation failures exceeded - crawl failed");
							m_State = State::Failed;
							return std::nullopt;
						}

						// Skip this page and try the next target
						m_Visited.insert(m_CurrentTarget);  // Mark as visited to skip it
						m_Failed.insert(m_CurrentTarget);   // ...and record it as unreachable
					}

					NavigateToNextTarget();

					if (m_State == State::NavigatingToNext)
					{
						return m_Navigator.OnPageUpdate(content, highlighted_line);
					}
					return std::nullopt;
				}
			}

			return cmd;
		}

		case State::CapturingPage:
		{
			// Deliver page content to the policy
			if (m_Policy)
			{
				m_Policy->OnPageReached(m_CurrentTarget, content);
			}

			// Track visit for multi-instance vs normal pages
			if (const MenuPage* page_info = m_Model.GetPage(m_CurrentTarget); page_info && page_info->multi_instance && m_CurrentMultiEdge.has_value())
			{
				// Mark the specific edge as visited (not the page itself)
				auto edge = m_CurrentMultiEdge.value();
				m_VisitedMultiEdges[m_CurrentTarget].insert({ edge->source, edge->trigger_line });
				m_CurrentMultiEdge = std::nullopt;

				LogDebug(Channel::Scraping, std::format("SpiderEngine: Captured multi-instance page {} via edge (source={}, line={}) ({} edges visited)",
					std::to_underlying(m_CurrentTarget),
					std::to_underlying(edge->source), edge->trigger_line,
					m_VisitedMultiEdges[m_CurrentTarget].size()));

				// Only mark the page as fully visited when ALL incoming edges are done
				if (!HasUnvisitedMultiEdges(m_CurrentTarget))
				{
					m_Visited.insert(m_CurrentTarget);
					LogInfo(Channel::Scraping, std::format("SpiderEngine: All instances of multi-instance page {} visited",
						std::to_underlying(m_CurrentTarget)));
				}
			}
			else
			{
				m_Visited.insert(m_CurrentTarget);

				LogDebug(Channel::Scraping, std::format("SpiderEngine: Captured page {} ({} pages visited so far)",
					std::to_underlying(m_CurrentTarget), m_Visited.size()));
			}

			// Choose next target
			NavigateToNextTarget();

			if (m_State == State::NavigatingToNext)
			{
				return m_Navigator.OnPageUpdate(content, highlighted_line);
			}
			return std::nullopt;
		}
		}

		return std::nullopt;
	}

	void SpiderEngine::OnStatusReceived()
	{
		// NOTE: Do NOT call m_Navigator.OnStatusMessageReceived() here.
		// The caller (Slot_OneTouch_Status) already calls it directly.
		// Calling it here too would double-decrement the pending counter,
		// causing the Navigator to check for page transitions too early.
	}

	std::optional<PageId> SpiderEngine::ChooseNextTarget() const
	{
		PageId current = m_Navigator.GetCurrentPage();
		if (current == PageId::Unknown)
		{
			return std::nullopt;
		}

		// Find the nearest unvisited page that the policy wants to visit
		// Use BFS from current position, filtering by policy
		const auto& all_pages = m_Model.GetAllPages();

		// Collect candidate pages
		std::vector<PageId> candidates;
		for (const auto& [id, page] : all_pages)
		{
			if (id == PageId::Unknown)
			{
				continue;
			}

			// For multi-instance pages, check if there are unvisited incoming edges
			if (page.multi_instance)
			{
				if (HasUnvisitedMultiEdges(id) && m_Policy && m_Policy->ShouldVisit(id, page))
				{
					candidates.push_back(id);
				}
				continue;
			}

			// Normal pages: skip if already visited
			if (m_Visited.count(id) > 0)
			{
				continue;
			}

			if (m_Policy && m_Policy->ShouldVisit(id, page))
			{
				candidates.push_back(id);
			}
		}

		if (candidates.empty())
		{
			return std::nullopt;
		}

		// Find the nearest candidate by path length
		PageId nearest = PageId::Unknown;
		size_t shortest_path = SIZE_MAX;

		for (PageId candidate : candidates)
		{
			// For multi-instance pages, compute path to the parent page (via specific edge)
			if (const MenuPage* page = m_Model.GetPage(candidate); page && page->multi_instance)
			{
				if (auto next_edge = GetNextUnvisitedMultiEdge(candidate); next_edge.has_value())
				{
					auto path = m_Model.FindPath(current, next_edge.value()->source);
					// Path to parent + 1 step to select item
					size_t total = path.empty() ? (current == next_edge.value()->source ? 1 : SIZE_MAX) : path.size() + 1;
					if (total < shortest_path)
					{
						shortest_path = total;
						nearest = candidate;
					}
				}
				continue;
			}

			auto path = m_Model.FindPath(current, candidate);
			if (!path.empty() && path.size() < shortest_path)
			{
				shortest_path = path.size();
				nearest = candidate;
			}
			else if (current == candidate)
			{
				// Already on this page
				return candidate;
			}
		}

		if (nearest != PageId::Unknown)
		{
			LogTrace(Channel::Scraping, std::format("SpiderEngine: Nearest unvisited target is page {} ({} steps away)",
				std::to_underlying(nearest), shortest_path));
		}

		return (nearest != PageId::Unknown) ? std::optional<PageId>(nearest) : std::nullopt;
	}

	void SpiderEngine::NavigateToNextTarget()
	{
		auto next = ChooseNextTarget();

		if (!next.has_value())
		{
			// All target pages visited
			LogInfo(Channel::Scraping, std::format("SpiderEngine: Crawl complete - {} pages visited",
				m_Visited.size()));
			if (m_Policy)
			{
				m_Policy->OnCrawlComplete();
			}
			m_State = State::Complete;
			return;
		}

		m_CurrentTarget = next.value();
		const MenuPage* target_page = m_Model.GetPage(m_CurrentTarget);

		LogDebug(Channel::Scraping, std::format("SpiderEngine: Next target -> {}({})",
			std::to_underlying(m_CurrentTarget), target_page ? target_page->name : "Unknown"));

		// Multi-instance pages: navigate to parent page and select specific menu item
		if (target_page && target_page->multi_instance)
		{
			auto next_edge = GetNextUnvisitedMultiEdge(m_CurrentTarget);
			if (next_edge.has_value())
			{
				auto edge = next_edge.value();
				m_CurrentMultiEdge = edge;

				LogDebug(Channel::Scraping, std::format("SpiderEngine: Multi-instance navigation via parent {}({}) line {} '{}'",
					std::to_underlying(edge->source), edge->trigger_line,
					edge->trigger_line, edge->label));

				// Navigate to the parent page, position cursor on the item, and select it
				m_Navigator.NavigateToItem(edge->source, edge->trigger_line, edge->label, m_CurrentTarget);
				m_State = State::NavigatingToNext;
				return;
			}
		}

		// If already on the target page, go straight to capture
		if (m_Navigator.GetCurrentPage() == m_CurrentTarget)
		{
			LogDebug(Channel::Scraping, "SpiderEngine: Already on target page - capturing");
			m_State = State::CapturingPage;
			return;
		}

		m_Navigator.NavigateTo(m_CurrentTarget);
		m_State = State::NavigatingToNext;
	}

	bool SpiderEngine::HasUnvisitedMultiEdges(PageId page) const
	{
		auto incoming = m_Model.GetIncomingSelectEdges(page);
		if (incoming.empty())
		{
			return false;
		}

		auto it = m_VisitedMultiEdges.find(page);
		if (it == m_VisitedMultiEdges.end())
		{
			return true;  // No edges visited yet
		}

		const auto& visited = it->second;
		for (const auto* edge : incoming)
		{
			if (visited.count({ edge->source, edge->trigger_line }) == 0)
			{
				return true;
			}
		}

		return false;
	}

	std::optional<const MenuEdge*> SpiderEngine::GetNextUnvisitedMultiEdge(PageId page) const
	{
		auto incoming = m_Model.GetIncomingSelectEdges(page);

		auto it = m_VisitedMultiEdges.find(page);
		const std::set<MultiEdgeKey>* visited = (it != m_VisitedMultiEdges.end()) ? &it->second : nullptr;

		for (const auto* edge : incoming)
		{
			if (!visited || visited->count({ edge->source, edge->trigger_line }) == 0)
			{
				return edge;
			}
		}

		return std::nullopt;
	}

}
// namespace AqualinkAutomate::Navigation
