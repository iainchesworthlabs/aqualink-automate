#include "kernel/system_boards.h"

namespace AqualinkAutomate::Kernel
{

	std::string ToDisplayString(SystemBoards board)
	{
		using enum SystemBoards;

		switch (board)
		{
			case RS4_Only:    return "RS-4 Only";
			case RS6_Only:    return "RS-6 Only";
			case RS8_Only:    return "RS-8 Only";

			case RS2_6_Dual:  return "RS-2/6 Dual";
			case RS2_10_Dual: return "RS-2/10 Dual";
			case RS2_14_Dual: return "RS-2/14 Dual";
			case RS2_22_Dual: return "RS-2/22 Dual";
			case RS2_30_Dual: return "RS-2/30 Dual";

			case RS4_Combo:   return "RS-4 Combo";
			case RS6_Combo:   return "RS-6 Combo";
			case RS8_Combo:   return "RS-8 Combo";
			case RS12_Combo:  return "RS-12 Combo";
			case RS16_Combo:  return "RS-16 Combo";
			case RS24_Combo:  return "RS-24 Combo";
			case RS32_Combo:  return "RS-32 Combo";

			case PD4_Only:    return "PD-4 Only";
			case PD8_Only:    return "PD-8 Only";
			case PD4_Combo:   return "PD-4 Combo";
			case PD6_Combo:   return "PD-6 Combo";
			case PD8_Combo:   return "PD-8 Combo";

			case Unknown:     return "Unknown";
		}

		return "Unknown";
	}

}
// namespace AqualinkAutomate::Kernel
