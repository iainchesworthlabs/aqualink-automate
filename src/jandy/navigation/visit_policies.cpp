#include "navigation/visit_policies.h"

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Navigation
{

	// =========================================================================
	// FullDiscoveryVisitPolicy
	// =========================================================================

	FullDiscoveryVisitPolicy::FullDiscoveryVisitPolicy(
		PageVisitCallback on_page,
		CrawlCompleteCallback on_complete,
		bool skip_label_pages)
		: m_OnPage(std::move(on_page))
		, m_OnComplete(std::move(on_complete))
		, m_SkipLabelPages(skip_label_pages)
	{
	}

	bool FullDiscoveryVisitPolicy::IsLabelPage(PageId page)
	{
		// The "Label Aux" scraping sub-tree: the aux list, the per-aux detail page
		// (multi-instance), and the label-category leaves reachable from it.
		using enum PageId;
		switch (page)
		{
		case LabelAuxList:
		case LabelAux:
		case GeneralLabels:
		case LightLabels:
		case WaterfallLabels:
		case CustomLabel:
			return true;

		default:
			return false;
		}
	}

	bool FullDiscoveryVisitPolicy::ShouldVisit(PageId page, const MenuPage& info) const
	{
		// Visit all pages except special system pages
		switch (page)
		{
		case PageId::Unknown:
		case PageId::StartUp:
		case PageId::Service:
		case PageId::TimeOut:
		case PageId::EnterPassword:
			return false;

		default:
			break;
		}

		// When aux labels are already seeded (e.g. by a real iAqualink2 on the bus),
		// skip the slow Label Aux scrape sub-tree entirely.
		return !(m_SkipLabelPages && IsLabelPage(page));
	}

	void FullDiscoveryVisitPolicy::OnPageReached(PageId page, const Utility::ScreenDataPage& content)
	{
		LogDebug(Channel::Scraping, std::format("FullDiscoveryVisitPolicy: Visited page {}",
			static_cast<uint32_t>(page)));

		if (m_OnPage)
		{
			m_OnPage(page, content);
		}
	}

	void FullDiscoveryVisitPolicy::OnCrawlComplete()
	{
		LogInfo(Channel::Scraping, "FullDiscoveryVisitPolicy: Full discovery crawl complete");

		if (m_OnComplete)
		{
			m_OnComplete();
		}
	}

	// =========================================================================
	// TargetedVisitPolicy
	// =========================================================================

	TargetedVisitPolicy::TargetedVisitPolicy(
		std::set<PageId> target_pages,
		PageVisitCallback on_page,
		CrawlCompleteCallback on_complete)
		: m_TargetPages(std::move(target_pages))
		, m_OnPage(std::move(on_page))
		, m_OnComplete(std::move(on_complete))
	{
	}

	bool TargetedVisitPolicy::ShouldVisit(PageId page, const MenuPage& info) const
	{
		return m_TargetPages.count(page) > 0;
	}

	void TargetedVisitPolicy::OnPageReached(PageId page, const Utility::ScreenDataPage& content)
	{
		LogDebug(Channel::Scraping, std::format("TargetedVisitPolicy: Visited target page {}",
			static_cast<uint32_t>(page)));

		if (m_OnPage)
		{
			m_OnPage(page, content);
		}
	}

	void TargetedVisitPolicy::OnCrawlComplete()
	{
		LogInfo(Channel::Scraping, "TargetedVisitPolicy: All target pages visited");

		if (m_OnComplete)
		{
			m_OnComplete();
		}
	}

}
// namespace AqualinkAutomate::Navigation
