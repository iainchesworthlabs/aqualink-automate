#include "kernel/pool_configurations.h"

namespace AqualinkAutomate::Kernel
{

	std::string ToDisplayString(PoolConfigurations config)
	{
		switch (config)
		{
			case PoolConfigurations::SingleBody:               return "Single Body";
			case PoolConfigurations::DualBody_SharedEquipment: return "Dual Body (Shared Equipment)";
			case PoolConfigurations::DualBody_DualEquipment:   return "Dual Body (Dual Equipment)";
			case PoolConfigurations::Unknown:                  return "Unknown";
		}

		return "Unknown";
	}

}
// namespace AqualinkAutomate::Kernel
