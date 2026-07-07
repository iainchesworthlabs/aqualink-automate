#pragma once

#include <functional>
#include <set>

#include "navigation/spider_engine.h"
#include "navigation/menu_model.h"
#include "utility/screen_data_page.h"

namespace AqualinkAutomate::Navigation
{

	using PageVisitCallback = std::function<void(PageId page, const Utility::ScreenDataPage& content)>;
	using CrawlCompleteCallback = std::function<void()>;

	// Visits all pages reachable from the starting position.
	//
	// When skip_label_pages is set, the "Label Aux" sub-tree (LabelAuxList /
	// LabelAux and its label sub-pages) is excluded from the crawl. This is used
	// by the emulated OneTouch when the DataHub already carries aux labels seeded
	// passively by a real iAqualink2 (AqualinkTouch 0x33) — re-scraping those
	// labels would walk ~36 pages under the 30s watchdog for data we already hold.
	// All other crawl targets (setpoints, equipment, diagnostics, etc.) are
	// unaffected, and the full label scrape still runs when no labels are seeded.
	class FullDiscoveryVisitPolicy : public VisitPolicy
	{
	public:
		explicit FullDiscoveryVisitPolicy(
			PageVisitCallback on_page = nullptr,
			CrawlCompleteCallback on_complete = nullptr,
			bool skip_label_pages = false);

		bool ShouldVisit(PageId page, const MenuPage& info) const override;
		void OnPageReached(PageId page, const Utility::ScreenDataPage& content) override;
		void OnCrawlComplete() override;

		// Returns true when this policy excludes the Label Aux scraping sub-tree.
		bool SkipsLabelPages() const { return m_SkipLabelPages; }

		// True when the given page is part of the Label Aux scraping sub-tree.
		static bool IsLabelPage(PageId page);

	private:
		PageVisitCallback m_OnPage;
		CrawlCompleteCallback m_OnComplete;
		bool m_SkipLabelPages{ false };
	};

	// Visits a specific set of pages (for startup scraping or targeted data collection)
	class TargetedVisitPolicy : public VisitPolicy
	{
	public:
		explicit TargetedVisitPolicy(
			std::set<PageId> target_pages,
			PageVisitCallback on_page = nullptr,
			CrawlCompleteCallback on_complete = nullptr);

		bool ShouldVisit(PageId page, const MenuPage& info) const override;
		void OnPageReached(PageId page, const Utility::ScreenDataPage& content) override;
		void OnCrawlComplete() override;

	private:
		std::set<PageId> m_TargetPages;
		PageVisitCallback m_OnPage;
		CrawlCompleteCallback m_OnComplete;
	};

}
// namespace AqualinkAutomate::Navigation
