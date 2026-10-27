// Object IDs of joystick.xml, written by tools/pool_export.py: do not edit.
#pragma once

#include <cstdint>

namespace joystick_pool
{
	constexpr std::uint16_t joystick_WorkingSet = 0;
	constexpr std::uint16_t joystick_DataMask = 1000;
	constexpr std::uint16_t stickX_AuxInput = 3000;
	constexpr std::uint16_t stickY_AuxInput = 3001;
	constexpr std::uint16_t buttonA_AuxInput = 3010;
	constexpr std::uint16_t buttonB_AuxInput = 3011;
	constexpr std::uint16_t buttonC_AuxInput = 3012;
	constexpr std::uint16_t buttonD_AuxInput = 3013;
	constexpr std::uint16_t buttonOk_AuxInput = 3014;
	constexpr std::uint16_t designator_OutStr = 11000;
	constexpr std::uint16_t title_OutStr = 11001;
	constexpr std::uint16_t stickX_OutStr = 11010;
	constexpr std::uint16_t stickY_OutStr = 11011;
	constexpr std::uint16_t buttonA_OutStr = 11012;
	constexpr std::uint16_t buttonB_OutStr = 11013;
	constexpr std::uint16_t buttonC_OutStr = 11014;
	constexpr std::uint16_t buttonD_OutStr = 11015;
	constexpr std::uint16_t buttonOk_OutStr = 11016;
	constexpr std::uint16_t title_Font = 23000;
	constexpr std::uint16_t label_Font = 23001;
}
