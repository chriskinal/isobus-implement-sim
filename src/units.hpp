// Profile units to DDI units and back (docs/MODEL.md sections 3 and 4).
#pragma once

#include <cstdint>
#include <string>

namespace sim
{
	/// @brief The scale that turns a rate in the profile's unit into the DDI's integer.
	/// l/ha is 0.01 mm³/m² (times 10 000), kg/ha is mg/m² (times 100), seeds/ha is 0.001/m² (times 0.1).
	inline double rate_scale(const std::string &unit)
	{
		if (unit == "l/ha")
		{
			return 10000.0;
		}
		if (unit == "kg/ha")
		{
			return 100.0;
		}
		return 0.1; // seeds/ha
	}

	/// @brief The scale that turns a bin level in the profile's unit into the content DDI's integer (ml, g, count).
	inline double content_scale(const std::string &binUnit)
	{
		if (binUnit == "l" || binUnit == "kg")
		{
			return 1000.0;
		}
		return 1.0; // seeds
	}

	/// @brief The trip totals 80 (l), 81 (kg) and 82 (count) are in whole profile units.
	inline double total_scale(const std::string &)
	{
		return 1.0;
	}

	inline std::int32_t to_ddi(double value, double scale)
	{
		const double v = value * scale;
		if (v > 2147483647.0)
		{
			return 2147483647;
		}
		if (v < -2147483648.0)
		{
			return -2147483647 - 1;
		}
		return static_cast<std::int32_t>(v + (v >= 0 ? 0.5 : -0.5));
	}

	constexpr double MM3_PER_S_PER_L_PER_MIN = 1000000.0 / 60.0; ///< DDI 37
	constexpr double PA_PER_BAR = 100000.0; ///< DDIs 193, 194
}
