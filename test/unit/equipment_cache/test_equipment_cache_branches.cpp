#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include "equipment_cache/equipment_cache_service.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/pool_configurations.h"
#include "kernel/system_boards.h"
#include "options/options_equipment_options.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
namespace Traits = Kernel::AuxillaryTraitsTypes;

//=============================================================================
// The equipment cache file is untrusted input: it is written by an earlier
// build, may have been hand-edited, and is read BEFORE live discovery corrects
// anything.  Every field is therefore optional and individually type-checked -
// a malformed entry must be skipped, never abort the restore, and never
// fabricate state that live discovery would have to fight.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_EquipmentCacheBranches, Test::HubLocatorInjector)

//-----------------------------------------------------------------------------
// Document-level shape
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(EquipCacheBranches_NonObjectOrDevicelessDocumentsIgnored)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;   // no file => in-memory only
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	// Not an object at all.
	BOOST_CHECK_NO_THROW(service.ApplySnapshot(nlohmann::json::array({ 1, 2, 3 })));
	BOOST_CHECK_NO_THROW(service.ApplySnapshot(nlohmann::json("a string")));

	// An object with no devices key, and one whose devices is the wrong type.
	BOOST_CHECK_NO_THROW(service.ApplySnapshot(nlohmann::json::object()));
	BOOST_CHECK_NO_THROW(service.ApplySnapshot(nlohmann::json{ { "devices", "not-an-array" } }));
	BOOST_CHECK_NO_THROW(service.ApplySnapshot(nlohmann::json{ { "devices", 17 } }));

	BOOST_CHECK(hub->Devices.FindByLabel("anything").empty());
	BOOST_CHECK(Kernel::PoolConfigurations::Unknown == hub->PoolConfiguration);
	BOOST_CHECK(Kernel::SystemBoards::Unknown == hub->SystemBoard);
}

//-----------------------------------------------------------------------------
// Per-device entry validation
//-----------------------------------------------------------------------------

// A device entry without a usable label cannot be identified, so it is skipped
// - and one bad entry must not prevent the good ones from being restored.
BOOST_AUTO_TEST_CASE(EquipCacheBranches_EntriesWithoutUsableLabelAreSkipped)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	nlohmann::json snapshot;
	snapshot["devices"] = nlohmann::json::array({
		42,                                                    // not an object
		nlohmann::json::object(),                              // no label
		{ { "type", "Auxillary" } },                           // still no label
		{ { "label", 7 } },                                    // label is not a string
		{ { "label", nlohmann::json() } },                     // label is null
		{ { "label", "Good Device" }, { "type", "Auxillary" } }
	});

	BOOST_CHECK_NO_THROW(service.ApplySnapshot(snapshot));

	BOOST_CHECK_EQUAL(hub->Devices.FindByLabel("Good Device").size(), 1u);
}

// Optional fields carried at the wrong type are ignored individually: the
// device is still restored from its label, just without those traits.
BOOST_AUTO_TEST_CASE(EquipCacheBranches_WrongTypedOptionalFieldsIgnored)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	nlohmann::json snapshot;
	snapshot["devices"] = nlohmann::json::array({
		{
			{ "label", "Pool Light" },
			{ "id", 12345 },                 // not a string -> no id, dedup by label
			{ "type", 5 },                   // not a string
			{ "body_of_water", true },       // not a string
			{ "hardware_id", 99 }            // not a string
		}
	});

	BOOST_CHECK_NO_THROW(service.ApplySnapshot(snapshot));

	auto restored = hub->Devices.FindByLabel("Pool Light");
	BOOST_REQUIRE_EQUAL(restored.size(), 1u);
	BOOST_CHECK(!restored.front()->AuxillaryTraits.Has(Traits::AuxillaryTypeTrait{}));
	BOOST_CHECK(!restored.front()->AuxillaryTraits.Has(Traits::BodyOfWaterTrait{}));
	BOOST_CHECK(!restored.front()->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}));
}

// Enum tokens that this build does not recognise (an older/newer cache, or an
// edited file) are dropped rather than guessed at.
BOOST_AUTO_TEST_CASE(EquipCacheBranches_UnrecognisedEnumTokensDropped)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	nlohmann::json snapshot;
	snapshot["devices"] = nlohmann::json::array({
		{
			{ "label", "Mystery Device" },
			{ "type", "SomeFutureType" },
			{ "body_of_water", "Lagoon" },
			{ "hardware_id", "AUX9" }
		}
	});

	BOOST_CHECK_NO_THROW(service.ApplySnapshot(snapshot));

	auto restored = hub->Devices.FindByLabel("Mystery Device");
	BOOST_REQUIRE_EQUAL(restored.size(), 1u);
	BOOST_CHECK(!restored.front()->AuxillaryTraits.Has(Traits::AuxillaryTypeTrait{}));
	BOOST_CHECK(!restored.front()->AuxillaryTraits.Has(Traits::BodyOfWaterTrait{}));
	// ...but a free-form hardware id is a plain string and IS kept.
	BOOST_REQUIRE(restored.front()->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}));
	BOOST_CHECK_EQUAL("AUX9", *(restored.front()->AuxillaryTraits[Traits::HardwareLabelTrait{}]));
}

//-----------------------------------------------------------------------------
// Configuration adoption
//-----------------------------------------------------------------------------

// The cached pool configuration / system board are adopted only when they are
// a recognised, non-Unknown token. Anything else leaves the hub untouched so
// live discovery is not pre-empted with junk.
BOOST_AUTO_TEST_CASE(EquipCacheBranches_ConfigurationOnlyAdoptedForKnownTokens)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	BOOST_REQUIRE(Kernel::PoolConfigurations::Unknown == hub->PoolConfiguration);
	BOOST_REQUIRE(Kernel::SystemBoards::Unknown == hub->SystemBoard);

	// Wrong types.
	service.ApplySnapshot(nlohmann::json{ { "pool_configuration", 3 }, { "system_board", false } });
	BOOST_CHECK(Kernel::PoolConfigurations::Unknown == hub->PoolConfiguration);
	BOOST_CHECK(Kernel::SystemBoards::Unknown == hub->SystemBoard);

	// Unrecognised tokens.
	service.ApplySnapshot(nlohmann::json{ { "pool_configuration", "NotAConfiguration" }, { "system_board", "NotABoard" } });
	BOOST_CHECK(Kernel::PoolConfigurations::Unknown == hub->PoolConfiguration);
	BOOST_CHECK(Kernel::SystemBoards::Unknown == hub->SystemBoard);

	// An explicit "Unknown" is a recognised token but carries no information.
	service.ApplySnapshot(nlohmann::json{ { "pool_configuration", "Unknown" }, { "system_board", "Unknown" } });
	BOOST_CHECK(Kernel::PoolConfigurations::Unknown == hub->PoolConfiguration);
	BOOST_CHECK(Kernel::SystemBoards::Unknown == hub->SystemBoard);

	// A real token is adopted.
	service.ApplySnapshot(nlohmann::json{ { "system_board", "RS8_Combo" } });
	BOOST_CHECK(Kernel::SystemBoards::RS8_Combo == hub->SystemBoard);

	// ...and once known it is never overwritten by a later cache read.
	service.ApplySnapshot(nlohmann::json{ { "system_board", "RS4_Only" } });
	BOOST_CHECK(Kernel::SystemBoards::RS8_Combo == hub->SystemBoard);
}

//-----------------------------------------------------------------------------
// Snapshot round-trip through the hostile-input path
//-----------------------------------------------------------------------------

// Applying a snapshot twice must be idempotent: the second pass finds every
// device already present (by stable id) and adds nothing.
BOOST_AUTO_TEST_CASE(EquipCacheBranches_ReapplyingASnapshotAddsNothing)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	auto device = std::make_shared<Kernel::AuxillaryDevice>();
	device->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
	device->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ "Waterfall" });
	device->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, std::string{ "AUX3" });
	hub->Devices.Add(device);

	const auto snapshot = service.Snapshot();

	Test::HubLocatorInjector fresh;
	EquipmentCache::EquipmentCacheService restorer(io, fresh, settings);

	restorer.ApplySnapshot(snapshot);
	restorer.ApplySnapshot(snapshot);

	BOOST_CHECK_EQUAL(fresh.Find<Kernel::DataHub>()->Devices.FindByLabel("Waterfall").size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()
