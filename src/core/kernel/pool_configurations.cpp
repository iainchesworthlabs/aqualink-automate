#include "kernel/pool_configurations.h"

namespace AqualinkAutomate::Kernel
{

	std::string ToDisplayString(PoolConfigurations config)
	{
		using enum PoolConfigurations;

		switch (config)
		{
			case SingleBody:               return "Single Body";
			case DualBody_SharedEquipment: return "Dual Body (Shared Equipment)";
			case DualBody_DualEquipment:   return "Dual Body (Dual Equipment)";
			case Unknown:                  return "Unknown";
		}

		return "Unknown";
	}

}
// namespace AqualinkAutomate::Kernel
