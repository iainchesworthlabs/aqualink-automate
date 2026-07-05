#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <magic_enum/magic_enum.hpp>

#include "equipment_cache/equipment_cache_service.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::EquipmentCache
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	namespace
	{
		// The config rarely changes; checking once a minute (and only writing on a
		// fingerprint change) keeps the cache fresh without churning the disk.
		constexpr std::chrono::seconds SAVE_CHECK_INTERVAL{ 60 };
	}
	// unnamed namespace

	EquipmentCacheService::EquipmentCacheService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::Equipment::EquipmentSettings& settings) :
		m_Settings(settings),
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_Timer(io_context)
	{
	}

	nlohmann::json EquipmentCacheService::Snapshot() const
	{
		nlohmann::json json;
		if (!m_DataHub) { return json; }

		json["pool_configuration"] = std::string{ magic_enum::enum_name(m_DataHub->PoolConfiguration) };
		json["system_board"] = std::string{ magic_enum::enum_name(m_DataHub->SystemBoard) };

		nlohmann::json devices = nlohmann::json::array();
		for (const auto& device : m_DataHub->Devices.FindByTrait(Traits::AuxillaryTypeTrait{}))
		{
			if (nullptr == device) { continue; }

			// Never persist an identity-less generic auxillary: a device of type Auxillary that
			// carries no HardwareLabelTrait cannot be reconciled by stable id on restore and would
			// resurrect as a duplicate. Named devices (pumps/heaters/chlorinators) reconcile by their
			// distinct label and properly identified auxes carry the hardware label, so both survive.
			if (Traits::AuxillaryTypes::Auxillary == *(device->AuxillaryTraits[Traits::AuxillaryTypeTrait{}])
				&& !device->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}))
			{
				continue;
			}

			nlohmann::json d;
			d["id"] = boost::uuids::to_string(device->Id());

			if (device->AuxillaryTraits.Has(Traits::LabelTrait{}))
			{
				d["label"] = std::string{ *(device->AuxillaryTraits[Traits::LabelTrait{}]) };
			}
			if (device->AuxillaryTraits.Has(Traits::AuxillaryTypeTrait{}))
			{
				d["type"] = std::string{ magic_enum::enum_name(*(device->AuxillaryTraits[Traits::AuxillaryTypeTrait{}])) };
			}
			if (device->AuxillaryTraits.Has(Traits::BodyOfWaterTrait{}))
			{
				d["body_of_water"] = std::string{ magic_enum::enum_name(*(device->AuxillaryTraits[Traits::BodyOfWaterTrait{}])) };
			}
			if (device->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}))
			{
				d["hardware_id"] = std::string{ *(device->AuxillaryTraits[Traits::HardwareLabelTrait{}]) };
			}

			devices.push_back(std::move(d));
		}
		json["devices"] = std::move(devices);
		return json;
	}

	void EquipmentCacheService::ApplySnapshot(const nlohmann::json& json) const
	{
		if (!m_DataHub || !json.is_object()) { return; }

		// Pool config / board: only adopt the cached value while still Unknown, so
		// live discovery (which runs after Load) always wins.
		if (json.contains("pool_configuration") && json["pool_configuration"].is_string())
		{
			if (auto cfg = magic_enum::enum_cast<Kernel::PoolConfigurations>(json["pool_configuration"].get<std::string>());
				cfg.has_value() && cfg.value() != Kernel::PoolConfigurations::Unknown && m_DataHub->PoolConfiguration == Kernel::PoolConfigurations::Unknown)
			{
				m_DataHub->ApplyPoolConfiguration(cfg.value(), Kernel::ConfigurationSource::Auto);
			}
		}
		if (json.contains("system_board") && json["system_board"].is_string())
		{
			if (auto board = magic_enum::enum_cast<Kernel::SystemBoards>(json["system_board"].get<std::string>());
				board.has_value() && board.value() != Kernel::SystemBoards::Unknown && m_DataHub->SystemBoard == Kernel::SystemBoards::Unknown)
			{
				m_DataHub->SystemBoard = board.value();
			}
		}

		if (!json.contains("devices") || !json["devices"].is_array()) { return; }

		std::size_t restored{ 0 };
		for (const auto& d : json["devices"])
		{
			if (!d.contains("label") || !d["label"].is_string()) { continue; }
			const std::string label = d["label"].get<std::string>();

			// Reconstruct the (stable) id up front so we can dedup on identity, not label.
			// A cached device and a live-discovered device for the same hardware share a
			// deterministic id (see DeriveStableUuid), so id is the reliable key; with
			// distinct ids per aux, same-label devices are each restored rather than
			// collapsed into one. Fall back to label only when no id was persisted.
			std::optional<boost::uuids::uuid> id;
			if (d.contains("id") && d["id"].is_string())
			{
				try
				{
					boost::uuids::string_generator gen;
					id = gen(d["id"].get<std::string>());
				}
				catch (const std::exception&)
				{
					// Malformed id: treat as no-id (dedup by label, fresh random id).
				}
			}

			// Skip if this identity is already present (live discovery ran first, or a
			// duplicate cache entry).
			if (id.has_value())
			{
				if (nullptr != m_DataHub->Devices.FindById(id.value())) { continue; }
			}
			else if (!m_DataHub->Devices.FindByLabel(label).empty())
			{
				continue;
			}

			auto device = id.has_value()
				? std::make_shared<Kernel::AuxillaryDevice>(id.value())
				: std::make_shared<Kernel::AuxillaryDevice>();

			device->AuxillaryTraits.Set(Traits::LabelTrait{}, label);

			if (d.contains("type") && d["type"].is_string())
			{
				if (auto type = magic_enum::enum_cast<Traits::AuxillaryTypes>(d["type"].get<std::string>()); type.has_value())
				{
					device->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type.value());
				}
			}
			if (d.contains("body_of_water") && d["body_of_water"].is_string())
			{
				if (auto body = magic_enum::enum_cast<Kernel::BodyOfWaterIds>(d["body_of_water"].get<std::string>()); body.has_value())
				{
					device->AuxillaryTraits.Set(Traits::BodyOfWaterTrait{}, body.value());
				}
			}
			if (d.contains("hardware_id") && d["hardware_id"].is_string())
			{
				device->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, d["hardware_id"].get<std::string>());
			}

			m_DataHub->Devices.Add(std::move(device));
			++restored;
		}

		LogInfo(Channel::Main, std::format("Equipment cache restored {} device(s)", restored));
	}

	void EquipmentCacheService::Load()
	{
		if (m_Settings.equipment_cache_file.empty() || !std::filesystem::exists(m_Settings.equipment_cache_file))
		{
			return;
		}

		try
		{
			std::ifstream in(m_Settings.equipment_cache_file, std::ios::binary);
			nlohmann::json json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
			ApplySnapshot(json);
			m_LastFingerprint = Fingerprint();
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Main, [&ex] { return std::format("Failed to load equipment cache: {}", ex.what()); });
		}
	}

	void EquipmentCacheService::SaveNow() const
	{
		if (m_Settings.equipment_cache_file.empty() || !m_DataHub) { return; }

		try
		{
			const std::filesystem::path target(m_Settings.equipment_cache_file);
			const std::filesystem::path tmp = target.string() + ".tmp";
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out << Snapshot().dump(2);
			}
			std::filesystem::rename(tmp, target);
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Main, [&ex] { return std::format("Failed to save equipment cache: {}", ex.what()); });
		}
	}

	void EquipmentCacheService::Start()
	{
		if (m_Settings.equipment_cache_file.empty()) { return; }
		m_LastFingerprint = Fingerprint();
		ScheduleSave();
	}

	void EquipmentCacheService::Stop()
	{
		m_Stopped = true;
		m_Timer.cancel();
		SaveNow();
	}

	void EquipmentCacheService::ScheduleSave()
	{
		m_Timer.expires_after(SAVE_CHECK_INTERVAL);
		m_Timer.async_wait([this](const boost::system::error_code& ec)
		{
			if (ec == boost::asio::error::operation_aborted || m_Stopped) { return; }

			if (const auto fingerprint = Fingerprint(); fingerprint != m_LastFingerprint)
			{
				SaveNow();
				m_LastFingerprint = fingerprint;
			}

			ScheduleSave();
		});
	}

	std::string EquipmentCacheService::Fingerprint() const
	{
		if (!m_DataHub) { return {}; }

		std::vector<std::string> device_parts;
		for (const auto& device : m_DataHub->Devices.FindByTrait(Traits::AuxillaryTypeTrait{}))
		{
			if (nullptr == device) { continue; }
			std::string part;
			if (device->AuxillaryTraits.Has(Traits::LabelTrait{}))
			{
				part += std::string{ *(device->AuxillaryTraits[Traits::LabelTrait{}]) };
			}
			part += '/';
			if (device->AuxillaryTraits.Has(Traits::AuxillaryTypeTrait{}))
			{
				part += std::string{ magic_enum::enum_name(*(device->AuxillaryTraits[Traits::AuxillaryTypeTrait{}])) };
			}
			part += '/';
			if (device->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}))
			{
				part += std::string{ *(device->AuxillaryTraits[Traits::HardwareLabelTrait{}]) };
			}
			device_parts.push_back(std::move(part));
		}
		std::ranges::sort(device_parts);

		std::string fingerprint;
		fingerprint += std::string{ magic_enum::enum_name(m_DataHub->PoolConfiguration) };
		fingerprint += '|';
		fingerprint += std::string{ magic_enum::enum_name(m_DataHub->SystemBoard) };
		for (const auto& part : device_parts)
		{
			fingerprint += '|';
			fingerprint += part;
		}
		return fingerprint;
	}

}
// namespace AqualinkAutomate::EquipmentCache
