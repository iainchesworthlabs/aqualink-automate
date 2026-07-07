#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>

#include "kernel/pool_configurations.h"
#include "kernel/system_boards.h"

namespace AqualinkAutomate::Utility
{
	
	class PoolConfigurationDecoder
	{
		using ConfigKey = std::string;
		using ConfigType = std::tuple<Kernel::PoolConfigurations, Kernel::SystemBoards, uint8_t, uint8_t>;
		using ConfigMap = std::unordered_map<ConfigKey, ConfigType>;

		static const std::string UNKNOWN_POOL_CONFIG_KEY;
		static const ConfigMap JANDY_EQUIPMENT_CONFIGURATIONS;

	public:
		explicit PoolConfigurationDecoder(const std::string& panel_type);

		Kernel::PoolConfigurations Configuration() const;
		Kernel::SystemBoards SystemBoard() const;
		uint8_t AuxillaryCount() const;
		uint8_t PowerCenterCount() const;

	private:
		static const ConfigType& DecodePanelType(const std::string& panel_type);

	private:
		const ConfigType& m_PoolConfig;
	};	

}
// namespace AqualinkAutomate::Utility
