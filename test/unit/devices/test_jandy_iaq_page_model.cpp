#include <cstdint>
#include <optional>
#include <string>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/iaq/iaq_page_model.h"

using namespace AqualinkAutomate::Devices::IAQ;
using AqualinkAutomate::Messages::ButtonStatuses;

BOOST_AUTO_TEST_SUITE(Jandy_IAQ_PageModel_TestSuite)

// =============================================================================
// Page id + title
// =============================================================================

BOOST_AUTO_TEST_CASE(PageModel_DefaultState_IsEmpty)
{
	PageModel model;
	BOOST_CHECK_EQUAL(model.PageId(), 0x00);
	BOOST_CHECK(model.Title().empty());
	BOOST_CHECK(model.Buttons().empty());
	BOOST_CHECK(model.ScheduleRows().empty());
	BOOST_CHECK(model.DevicePickerRows().empty());
	BOOST_CHECK(model.SpaSwitchPickerRows().empty());
}

BOOST_AUTO_TEST_CASE(OnPageStart_LatchesIdAndTitle)
{
	PageModel model;
	model.SetTitle("Schedule Group A");
	model.OnPageStart(0x28);
	BOOST_CHECK_EQUAL(model.PageId(), 0x28);
	// OnPageStart clears the title accumulator (a fresh page re-pushes its own title).
	BOOST_CHECK(model.Title().empty());
}

// =============================================================================
// Button table + FindButtonByLabel (prefix match, trimmed)
// =============================================================================

BOOST_AUTO_TEST_CASE(Buttons_UpsertAndErase)
{
	PageModel model;
	model.UpsertButton(9, "Pool LightON", ButtonStatuses::On);
	model.UpsertButton(11, "Spillway", ButtonStatuses::Off);

	BOOST_REQUIRE_EQUAL(model.Buttons().size(), 2u);
	BOOST_CHECK_EQUAL(model.Buttons().at(9).name, "Pool LightON");
	BOOST_CHECK(model.Buttons().at(9).status == ButtonStatuses::On);

	// Re-upsert refreshes name + status in place.
	model.UpsertButton(9, "Pool LightOFF", ButtonStatuses::Off);
	BOOST_CHECK_EQUAL(model.Buttons().at(9).name, "Pool LightOFF");
	BOOST_CHECK(model.Buttons().at(9).status == ButtonStatuses::Off);

	model.EraseButton(11);
	BOOST_CHECK_EQUAL(model.Buttons().size(), 1u);
	BOOST_CHECK(!model.Buttons().contains(11));
}

BOOST_AUTO_TEST_CASE(FindButtonByLabel_PrefixMatchesTrailingStatusSuffix)
{
	PageModel model;
	model.UpsertButton(9, "Pool LightON", ButtonStatuses::On);   // home-page status suffix
	model.UpsertButton(11, "Spillway", ButtonStatuses::Off);

	// "Pool Light" is a prefix of "Pool LightON".
	auto idx = model.FindButtonByLabel("Pool Light");
	BOOST_REQUIRE(idx.has_value());
	BOOST_CHECK_EQUAL(idx.value(), 9);

	// Exact label still matches.
	BOOST_CHECK_EQUAL(model.FindButtonByLabel("Spillway").value(), 11);

	// Surrounding whitespace on the query is trimmed before matching.
	BOOST_CHECK_EQUAL(model.FindButtonByLabel("  Spillway  ").value(), 11);
}

BOOST_AUTO_TEST_CASE(FindButtonByLabel_NoMatchAndEmptyLabel)
{
	PageModel model;
	model.UpsertButton(9, "Pool LightON", ButtonStatuses::On);

	BOOST_CHECK(!model.FindButtonByLabel("Spa Heat").has_value());
	// An empty (or whitespace-only) label never matches -- avoids matching every button.
	BOOST_CHECK(!model.FindButtonByLabel("").has_value());
	BOOST_CHECK(!model.FindButtonByLabel("   ").has_value());
}

// =============================================================================
// Row accumulators + OnPageStart invalidation semantics
// =============================================================================

BOOST_AUTO_TEST_CASE(OnPageStart_ClearsScheduleAndDevicePickerRows_ButNotButtonsOrSpaSwitchPicker)
{
	PageModel model;
	model.UpsertButton(0, "Filter Pump", ButtonStatuses::On);
	model.SetScheduleRow(1, "Pool 9:00 AM 5:00 PM All");
	model.SetDevicePickerRow(1, "Pool Light");
	model.SetSpaSwitchPickerRow(1, "Filter Pump");

	model.OnPageStart(0x01);

	// Schedule + device-picker accumulators reset on a fresh page...
	BOOST_CHECK(model.ScheduleRows().empty());
	BOOST_CHECK(model.DevicePickerRows().empty());
	// ...but buttons persist (refreshed/erased per PageButton frame) and the spa-switch picker
	// clears only on its own group-0x01 header row, NOT on PageStart. Preserve that behaviour.
	BOOST_CHECK_EQUAL(model.Buttons().size(), 1u);
	BOOST_CHECK_EQUAL(model.SpaSwitchPickerRows().size(), 1u);
}

BOOST_AUTO_TEST_CASE(SpaSwitchPickerRows_ClearIsExplicit)
{
	PageModel model;
	model.SetSpaSwitchPickerRow(1, "Filter Pump");
	model.SetSpaSwitchPickerRow(2, "Pool Light");
	BOOST_REQUIRE_EQUAL(model.SpaSwitchPickerRows().size(), 2u);

	model.ClearSpaSwitchPickerRows();
	BOOST_CHECK(model.SpaSwitchPickerRows().empty());
}

BOOST_AUTO_TEST_SUITE_END()
