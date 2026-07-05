#include "kernel/system_boards.h"

namespace AqualinkAutomate::Kernel
{

	std::string ToDisplayString(SystemBoards board)
	{
		switch (board)
		{
			case SystemBoards::RS4_Only:    return "RS-4 Only";
			case SystemBoards::RS6_Only:    return "RS-6 Only";
			case SystemBoards::RS8_Only:    return "RS-8 Only";

			case SystemBoards::RS2_6_Dual:  return "RS-2/6 Dual";
			case SystemBoards::RS2_10_Dual: return "RS-2/10 Dual";
			case SystemBoards::RS2_14_Dual: return "RS-2/14 Dual";
			case SystemBoards::RS2_22_Dual: return "RS-2/22 Dual";
			case SystemBoards::RS2_30_Dual: return "RS-2/30 Dual";

			case SystemBoards::RS4_Combo:   return "RS-4 Combo";
			case SystemBoards::RS6_Combo:   return "RS-6 Combo";
			case SystemBoards::RS8_Combo:   return "RS-8 Combo";
			case SystemBoards::RS12_Combo:  return "RS-12 Combo";
			case SystemBoards::RS16_Combo:  return "RS-16 Combo";
			case SystemBoards::RS24_Combo:  return "RS-24 Combo";
			case SystemBoards::RS32_Combo:  return "RS-32 Combo";

			case SystemBoards::PD4_Only:    return "PD-4 Only";
			case SystemBoards::PD8_Only:    return "PD-8 Only";
			case SystemBoards::PD4_Combo:   return "PD-4 Combo";
			case SystemBoards::PD6_Combo:   return "PD-6 Combo";
			case SystemBoards::PD8_Combo:   return "PD-8 Combo";

			case SystemBoards::Unknown:     return "Unknown";
		}

		return "Unknown";
	}

}
// namespace AqualinkAutomate::Kernel
