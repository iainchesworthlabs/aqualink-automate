#include <algorithm>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <magic_enum/magic_enum.hpp>

#include "kernel/system_boards.h"
#include "utility/jandy_pool_configuration_decoder.h"

using namespace AqualinkAutomate;

//=============================================================================
// SystemBoards display labels.
//
// ToDisplayString is not merely cosmetic: its strings are the canonical
// panel-type keys that PoolConfigurationDecoder decodes back into the same
// enum value (the header documents this round-trip).  A typo in one label
// would therefore silently break the decode of that panel model, so every
// enumerator is pinned here AND round-tripped through the decoder.
//=============================================================================

namespace
{

	struct BoardCase
	{
		Kernel::SystemBoards board;
		const char* label;
	};

	const std::vector<BoardCase>& AllBoards()
	{
		using enum Kernel::SystemBoards;

		static const std::vector<BoardCase> cases
		{
			{ RS4_Only,    "RS-4 Only" },
			{ RS6_Only,    "RS-6 Only" },
			{ RS8_Only,    "RS-8 Only" },

			{ RS2_6_Dual,  "RS-2/6 Dual" },
			{ RS2_10_Dual, "RS-2/10 Dual" },
			{ RS2_14_Dual, "RS-2/14 Dual" },
			{ RS2_22_Dual, "RS-2/22 Dual" },
			{ RS2_30_Dual, "RS-2/30 Dual" },

			{ RS4_Combo,   "RS-4 Combo" },
			{ RS6_Combo,   "RS-6 Combo" },
			{ RS8_Combo,   "RS-8 Combo" },
			{ RS12_Combo,  "RS-12 Combo" },
			{ RS16_Combo,  "RS-16 Combo" },
			{ RS24_Combo,  "RS-24 Combo" },
			{ RS32_Combo,  "RS-32 Combo" },

			{ PD4_Only,    "PD-4 Only" },
			{ PD8_Only,    "PD-8 Only" },
			{ PD4_Combo,   "PD-4 Combo" },
			{ PD6_Combo,   "PD-6 Combo" },
			{ PD8_Combo,   "PD-8 Combo" },

			{ Unknown,     "Unknown" }
		};

		return cases;
	}

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_KernelSystemBoards)

// Every enumerator has its own distinct, pinned label.
BOOST_AUTO_TEST_CASE(SystemBoards_EveryEnumeratorHasItsLabel)
{
	for (const auto& [board, label] : AllBoards())
	{
		BOOST_CHECK_EQUAL(std::string{ label }, Kernel::ToDisplayString(board));
	}
}

// The table above must stay exhaustive: if an enumerator is added without a
// label (and without a case in the switch), this catches it.
BOOST_AUTO_TEST_CASE(SystemBoards_TableCoversEveryEnumerator)
{
	BOOST_CHECK_EQUAL(magic_enum::enum_count<Kernel::SystemBoards>(), AllBoards().size());

	for (const auto board : magic_enum::enum_values<Kernel::SystemBoards>())
	{
		const auto label = Kernel::ToDisplayString(board);
		BOOST_CHECK_MESSAGE(!label.empty(), "no display label for " << magic_enum::enum_name(board));

		// Only the Unknown enumerator may render as "Unknown"; anything else
		// rendering that way means a missing switch case.
		if (Kernel::SystemBoards::Unknown != board)
		{
			BOOST_CHECK_MESSAGE(label != "Unknown", "missing display label for " << magic_enum::enum_name(board));
		}
	}
}

// No two boards share a label - the decoder keys on these strings, so a
// duplicate would make one panel model undecodable.
BOOST_AUTO_TEST_CASE(SystemBoards_LabelsAreUnique)
{
	std::vector<std::string> seen;

	for (const auto& [board, label] : AllBoards())
	{
		const auto rendered = Kernel::ToDisplayString(board);
		BOOST_CHECK_MESSAGE(std::find(seen.begin(), seen.end(), rendered) == seen.end(),
			"duplicate display label: " << rendered);
		seen.push_back(rendered);
	}
}

// The documented round-trip: the display label of a board decodes back to that
// same board through PoolConfigurationDecoder.
BOOST_AUTO_TEST_CASE(SystemBoards_LabelsRoundTripThroughThePanelDecoder)
{
	for (const auto& [board, label] : AllBoards())
	{
		const Utility::PoolConfigurationDecoder decoder(Kernel::ToDisplayString(board));

		BOOST_CHECK_MESSAGE(board == decoder.SystemBoard(),
			"label '" << label << "' did not decode back to " << magic_enum::enum_name(board));
	}
}

// An out-of-catalogue value (a corrupt cache entry, a future enumerator read
// from a snapshot) must fall back to "Unknown" rather than returning garbage.
BOOST_AUTO_TEST_CASE(SystemBoards_UnrecognisedValueFallsBackToUnknown)
{
	// 31 is inside the enum's representable range but is not an enumerator.
	const auto out_of_range = static_cast<Kernel::SystemBoards>(31);

	BOOST_CHECK_EQUAL("Unknown", Kernel::ToDisplayString(out_of_range));
}

BOOST_AUTO_TEST_SUITE_END()
