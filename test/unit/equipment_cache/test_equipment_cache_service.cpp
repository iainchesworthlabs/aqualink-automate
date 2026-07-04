#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include "equipment_cache/equipment_cache_service.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/stable_id.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "options/options_equipment_options.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
namespace Traits = Kernel::AuxillaryTraitsTypes;

namespace
{
	std::shared_ptr<Kernel::AuxillaryDevice> MakeDevice(const std::string& label, Traits::AuxillaryTypes type)
	{
		auto dev = std::make_shared<Kernel::AuxillaryDevice>();
		dev->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type);
		dev->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		return dev;
	}
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_EquipmentCache, Test::HubLocatorInjector)

BOOST_AUTO_TEST_CASE(SnapshotApply_RoundTripsConfigAndDevicesWithStableUuid)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;   // no file => in-memory
	EquipmentCache::EquipmentCacheService source(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::SingleBody, Kernel::ConfigurationSource::Auto);

	auto dev = MakeDevice("Aux1", Traits::AuxillaryTypes::Light);
	dev->AuxillaryTraits.Set(Traits::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Pool);
	const auto original_id = dev->Id();
	hub->Devices.Add(dev);

	const auto snapshot = source.Snapshot();
	BOOST_CHECK_EQUAL(snapshot["pool_configuration"], "SingleBody");
	BOOST_REQUIRE_EQUAL(snapshot["devices"].size(), 1u);
	BOOST_CHECK_EQUAL(snapshot["devices"][0]["label"], "Aux1");
	BOOST_CHECK_EQUAL(snapshot["devices"][0]["type"], "Light");

	// Apply into a fresh hub: config + device restored, UUID preserved.
	Test::HubLocatorInjector fresh;
	EquipmentCache::EquipmentCacheService dest(io, fresh, settings);
	dest.ApplySnapshot(snapshot);

	auto fresh_hub = fresh.Find<Kernel::DataHub>();
	BOOST_CHECK(fresh_hub->PoolConfiguration == Kernel::PoolConfigurations::SingleBody);

	auto restored = fresh_hub->Devices.FindByLabel("Aux1");
	BOOST_REQUIRE_EQUAL(restored.size(), 1u);
	BOOST_CHECK(restored.front()->Id() == original_id);   // stable id across "restart"
	BOOST_CHECK(restored.front()->AuxillaryTraits.Has(Traits::AuxillaryTypeTrait{}));
}

BOOST_AUTO_TEST_CASE(ApplySnapshot_MergesByLabel_NoDuplicate)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	hub->Devices.Add(MakeDevice("Aux1", Traits::AuxillaryTypes::Auxillary));

	// A cached snapshot for the SAME label (different uuid) must not duplicate it.
	nlohmann::json snapshot;
	snapshot["devices"] = nlohmann::json::array({
		{ { "id", boost::uuids::to_string(boost::uuids::random_generator()()) }, { "label", "Aux1" }, { "type", "Auxillary" } }
	});
	service.ApplySnapshot(snapshot);

	BOOST_CHECK_EQUAL(hub->Devices.FindByLabel("Aux1").size(), 1u);
}

BOOST_AUTO_TEST_CASE(Snapshot_PersistsHardwareId_AndApplyRestoresIt)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService source(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	auto dev = MakeDevice("Swim Jet", Traits::AuxillaryTypes::Auxillary);
	dev->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, std::string{ "Aux5" });
	hub->Devices.Add(dev);

	const auto snapshot = source.Snapshot();
	BOOST_REQUIRE_EQUAL(snapshot["devices"].size(), 1u);
	BOOST_CHECK_EQUAL(snapshot["devices"][0]["hardware_id"], "Aux5");

	Test::HubLocatorInjector fresh;
	EquipmentCache::EquipmentCacheService dest(io, fresh, settings);
	dest.ApplySnapshot(snapshot);

	auto restored = fresh.Find<Kernel::DataHub>()->Devices.FindByLabel("Swim Jet");
	BOOST_REQUIRE_EQUAL(restored.size(), 1u);
	BOOST_REQUIRE(restored.front()->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}));
	BOOST_CHECK_EQUAL(std::string{ *(restored.front()->AuxillaryTraits[Traits::HardwareLabelTrait{}]) }, "Aux5");
}

// Distinct auxes round-trip and each is reachable by the EXACT stable id live discovery
// reconciles on - this is what lets a cache-restored placeholder be found (and updated in
// place) rather than duplicated. (Same-LABEL devices collapse on restore via the device
// graph's type+label dedup; the no-duplicate guarantee for that case is proven end-to-end in
// test_navigation_aux_scrolling_discovery's Replay_LabelAux_AfterCacheRestore test, where
// live discovery reconciles by stable id.)
BOOST_AUTO_TEST_CASE(ApplySnapshot_DistinctDevices_RestoreWithStableIdAndHardwareId)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService source(io, *this, settings);

	struct Entry { const char* key; const char* label; const char* hardware; };
	const Entry entries[] = {
		{ "jandy:aux:1", "Swim Jet",  "Aux1" },
		{ "jandy:aux:2", "Spa Jet",   "Aux2" },
		{ "jandy:aux:3", "Waterfall", "Aux3" },
	};

	auto hub = Find<Kernel::DataHub>();
	for (const auto& e : entries)
	{
		auto dev = std::make_shared<Kernel::AuxillaryDevice>(Kernel::DeriveStableUuid(e.key));
		dev->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
		dev->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ e.label });
		dev->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, std::string{ e.hardware });
		hub->Devices.Add(dev);
	}

	const auto snapshot = source.Snapshot();
	BOOST_REQUIRE_EQUAL(snapshot["devices"].size(), 3u);

	Test::HubLocatorInjector fresh;
	EquipmentCache::EquipmentCacheService dest(io, fresh, settings);
	dest.ApplySnapshot(snapshot);

	auto fresh_hub = fresh.Find<Kernel::DataHub>();
	BOOST_CHECK_EQUAL(fresh_hub->Devices.FindByLabel("Swim Jet").size(), 1u);
	BOOST_CHECK_EQUAL(fresh_hub->Devices.FindByLabel("Spa Jet").size(), 1u);

	// Each restored device is reachable by the exact stable id live discovery computes.
	auto restored = fresh_hub->Devices.FindById(Kernel::DeriveStableUuid("jandy:aux:2"));
	BOOST_REQUIRE(nullptr != restored);
	BOOST_REQUIRE(restored->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}));
	BOOST_CHECK_EQUAL(std::string{ *(restored->AuxillaryTraits[Traits::HardwareLabelTrait{}]) }, "Aux2");
}

BOOST_AUTO_TEST_CASE(Snapshot_OmitsIdentityLessAuxillaryPlaceholder)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService source(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	// An identity-less generic auxillary (type Auxillary, no HardwareLabelTrait): the legacy
	// duplicate. It must NOT be persisted, else it resurrects on restore and republishes as a
	// duplicate (it cannot be reconciled by stable id).
	hub->Devices.Add(MakeDevice("Aux5", Traits::AuxillaryTypes::Auxillary));

	// A properly identified aux (carries the hardware label) and a named device (pump) must both
	// still be persisted.
	auto identified = MakeDevice("Pool Light", Traits::AuxillaryTypes::Auxillary);
	identified->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, std::string{ "Aux5" });
	hub->Devices.Add(identified);
	hub->Devices.Add(MakeDevice("Filter Pump", Traits::AuxillaryTypes::Pump));

	const auto snapshot = source.Snapshot();

	// Only the identified aux + the pump survive; the placeholder is dropped.
	BOOST_REQUIRE_EQUAL(snapshot["devices"].size(), 2u);
	bool saw_placeholder = false, saw_identified = false, saw_pump = false;
	for (const auto& d : snapshot["devices"])
	{
		const std::string label = d.value("label", "");
		if (label == "Aux5") { saw_placeholder = true; }
		if (label == "Pool Light") { saw_identified = true; }
		if (label == "Filter Pump") { saw_pump = true; }
	}
	BOOST_CHECK(!saw_placeholder);   // identity-less placeholder dropped
	BOOST_CHECK(saw_identified);     // identified aux kept
	BOOST_CHECK(saw_pump);           // named device kept
}

BOOST_AUTO_TEST_CASE(FileRoundTrip_SaveThenLoad)
{
	boost::asio::io_context io;
	const auto path = (std::filesystem::temp_directory_path() / "aqualink_eqcache_test.json").string();
	std::error_code ec;
	std::filesystem::remove(path, ec);

	Options::Equipment::EquipmentSettings settings;
	settings.equipment_cache_file = path;

	{
		EquipmentCache::EquipmentCacheService writer(io, *this, settings);
		Find<Kernel::DataHub>()->Devices.Add(MakeDevice("Filter Pump", Traits::AuxillaryTypes::Pump));
		writer.SaveNow();
	}

	Test::HubLocatorInjector fresh;
	EquipmentCache::EquipmentCacheService reader(io, fresh, settings);
	reader.Load();
	BOOST_CHECK_EQUAL(fresh.Find<Kernel::DataHub>()->Devices.FindByLabel("Filter Pump").size(), 1u);

	std::filesystem::remove(path, ec);
	std::filesystem::remove(path + ".tmp", ec);
}

//=============================================================================
// Load / SaveNow guards (missing file, malformed JSON, no configured path).
//=============================================================================

BOOST_AUTO_TEST_CASE(Load_FileDoesNotExist_IsNoOp)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	settings.equipment_cache_file = (std::filesystem::temp_directory_path() / "aqualink_eqcache_absent.json").string();
	std::error_code ec;
	std::filesystem::remove(settings.equipment_cache_file, ec);   // ensure absent

	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	BOOST_CHECK_NO_THROW(service.Load());
	BOOST_CHECK(Find<Kernel::DataHub>()->Devices.FindByTrait(Traits::AuxillaryTypeTrait{}).empty());
}

BOOST_AUTO_TEST_CASE(Load_MalformedJson_IsCaughtAndIgnored)
{
	boost::asio::io_context io;
	const auto path = (std::filesystem::temp_directory_path() / "aqualink_eqcache_bad.json").string();
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << "{ this is not valid json";
	}

	Options::Equipment::EquipmentSettings settings;
	settings.equipment_cache_file = path;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	// The parse exception is caught and logged; Load must not propagate it.
	BOOST_CHECK_NO_THROW(service.Load());
	BOOST_CHECK(Find<Kernel::DataHub>()->Devices.FindByTrait(Traits::AuxillaryTypeTrait{}).empty());

	std::error_code ec;
	std::filesystem::remove(path, ec);
}

BOOST_AUTO_TEST_CASE(SaveNow_NoConfiguredFile_IsNoOp)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;   // no equipment_cache_file
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	Find<Kernel::DataHub>()->Devices.Add(MakeDevice("Filter Pump", Traits::AuxillaryTypes::Pump));

	BOOST_CHECK_NO_THROW(service.SaveNow());
}

//=============================================================================
// ApplySnapshot config adoption: the cached pool-config / system-board is only
// adopted while the live value is still Unknown (live discovery always wins).
//=============================================================================

BOOST_AUTO_TEST_CASE(ApplySnapshot_AdoptsPoolConfig_WhenUnknown)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	BOOST_REQUIRE(hub->PoolConfiguration == Kernel::PoolConfigurations::Unknown);

	nlohmann::json snapshot;
	snapshot["pool_configuration"] = "SingleBody";
	service.ApplySnapshot(snapshot);

	BOOST_CHECK(hub->PoolConfiguration == Kernel::PoolConfigurations::SingleBody);
}

BOOST_AUTO_TEST_CASE(ApplySnapshot_IgnoresPoolConfig_WhenAlreadyKnown)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_SharedEquipment, Kernel::ConfigurationSource::Auto);

	nlohmann::json snapshot;
	snapshot["pool_configuration"] = "SingleBody";
	service.ApplySnapshot(snapshot);

	// Live discovery already set it, so the cached value must NOT override.
	BOOST_CHECK(hub->PoolConfiguration == Kernel::PoolConfigurations::DualBody_SharedEquipment);
}

BOOST_AUTO_TEST_CASE(ApplySnapshot_AdoptsSystemBoard_WhenUnknown)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	BOOST_REQUIRE(hub->SystemBoard == Kernel::SystemBoards::Unknown);

	nlohmann::json snapshot;
	snapshot["system_board"] = "RS8_Only";
	service.ApplySnapshot(snapshot);

	BOOST_CHECK(hub->SystemBoard == Kernel::SystemBoards::RS8_Only);
}

BOOST_AUTO_TEST_CASE(ApplySnapshot_MalformedDeviceId_RestoresWithFreshIdAndTraits)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	// A malformed "id" must not abort the restore: the device is recreated with a fresh
	// id (deduped by label instead) and its other traits are still applied.
	nlohmann::json snapshot;
	snapshot["devices"] = nlohmann::json::array({
		{ { "id", "not-a-uuid" }, { "label", "Spa Jet" }, { "type", "Auxillary" }, { "body_of_water", "Spa" } }
	});
	service.ApplySnapshot(snapshot);

	auto restored = hub->Devices.FindByLabel("Spa Jet");
	BOOST_REQUIRE_EQUAL(restored.size(), 1u);
	BOOST_REQUIRE(restored.front()->AuxillaryTraits.Has(Traits::BodyOfWaterTrait{}));
	BOOST_CHECK(*(restored.front()->AuxillaryTraits[Traits::BodyOfWaterTrait{}]) == Kernel::BodyOfWaterIds::Spa);
}

// A cached device WITHOUT a persisted id falls back to label-based dedup: if a
// device with that label is already present, the entry is skipped (no duplicate).
BOOST_AUTO_TEST_CASE(ApplySnapshot_NoIdDuplicateLabel_SkippedByLabelDedup)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	hub->Devices.Add(MakeDevice("Waterfall", Traits::AuxillaryTypes::Auxillary));

	// No "id" field at all -> label dedup path: the existing "Waterfall" is found so
	// the cached entry is dropped rather than duplicated.
	nlohmann::json snapshot;
	snapshot["devices"] = nlohmann::json::array({
		{ { "label", "Waterfall" }, { "type", "Auxillary" } }
	});
	service.ApplySnapshot(snapshot);

	BOOST_CHECK_EQUAL(hub->Devices.FindByLabel("Waterfall").size(), 1u);
}

// A cached device WITHOUT a persisted id and a NEW label is restored with a fresh
// random id (the no-id restore branch).
BOOST_AUTO_TEST_CASE(ApplySnapshot_NoIdNewLabel_RestoredWithFreshId)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();

	nlohmann::json snapshot;
	snapshot["devices"] = nlohmann::json::array({
		{ { "label", "Deck Jets" }, { "type", "Auxillary" } }
	});
	service.ApplySnapshot(snapshot);

	BOOST_CHECK_EQUAL(hub->Devices.FindByLabel("Deck Jets").size(), 1u);
}

// Start() seeds the fingerprint and schedules the save timer; Stop() cancels it
// and writes a final snapshot. Exercising this against a real file with a
// hardware-labelled device drives the Fingerprint hardware-label branch too.
BOOST_AUTO_TEST_CASE(StartStop_SchedulesAndWritesFinalSnapshot)
{
	boost::asio::io_context io;
	const auto path = (std::filesystem::temp_directory_path() / "aqualink_eqcache_startstop.json").string();
	std::error_code ec;
	std::filesystem::remove(path, ec);

	Options::Equipment::EquipmentSettings settings;
	settings.equipment_cache_file = path;

	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	auto hub = Find<Kernel::DataHub>();
	auto dev = MakeDevice("Pool Light", Traits::AuxillaryTypes::Auxillary);
	dev->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, std::string{ "Aux5" });
	hub->Devices.Add(dev);

	BOOST_CHECK_NO_THROW(service.Start());   // seeds fingerprint + schedules timer
	BOOST_CHECK_NO_THROW(service.Stop());    // cancels timer + final SaveNow

	// Stop() wrote the snapshot; reading it back restores the hardware-labelled device.
	BOOST_CHECK(std::filesystem::exists(path));

	Test::HubLocatorInjector fresh;
	EquipmentCache::EquipmentCacheService reader(io, fresh, settings);
	reader.Load();
	auto restored = fresh.Find<Kernel::DataHub>()->Devices.FindByLabel("Pool Light");
	BOOST_REQUIRE_EQUAL(restored.size(), 1u);
	BOOST_REQUIRE(restored.front()->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}));
	BOOST_CHECK_EQUAL(std::string{ *(restored.front()->AuxillaryTraits[Traits::HardwareLabelTrait{}]) }, "Aux5");

	std::filesystem::remove(path, ec);
	std::filesystem::remove(path + ".tmp", ec);
}

// Start() with an empty file path is a no-op (the cache is disabled).
BOOST_AUTO_TEST_CASE(Start_NoConfiguredFile_IsNoOp)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;   // no equipment_cache_file
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	BOOST_CHECK_NO_THROW(service.Start());
	BOOST_CHECK_NO_THROW(service.Stop());
}

BOOST_AUTO_TEST_CASE(Snapshot_FreshHub_HasUnknownConfigAndNoDevices)
{
	boost::asio::io_context io;
	Options::Equipment::EquipmentSettings settings;
	EquipmentCache::EquipmentCacheService service(io, *this, settings);

	const auto snapshot = service.Snapshot();

	BOOST_CHECK_EQUAL(snapshot["pool_configuration"], "Unknown");
	BOOST_CHECK_EQUAL(snapshot["system_board"], "Unknown");
	BOOST_REQUIRE(snapshot.contains("devices"));
	BOOST_CHECK(snapshot["devices"].empty());
}

BOOST_AUTO_TEST_SUITE_END()
