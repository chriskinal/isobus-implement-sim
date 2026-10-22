// The device descriptor object pool, built from a profile (docs/MODEL.md section 1,
// SEEDER.md and SPRAYER.md section 4).
#pragma once

#include "profile.hpp"

#include "isobus/isobus/can_NAME.hpp"
#include "isobus/isobus/isobus_device_descriptor_object_pool.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim
{
	/// @brief Which element number is which part of the machine, for the process data callbacks.
	struct ElementMap
	{
		std::uint16_t device = 0;
		std::uint16_t connector = 1;
		std::vector<std::uint16_t> booms;
		std::vector<std::vector<std::uint16_t>> sections;
		std::vector<std::uint16_t> bins;

		bool boom_for(std::uint16_t element, std::size_t &boom) const;
		bool section_for(std::uint16_t element, std::size_t &boom, std::size_t &index) const;
		bool bin_for(std::uint16_t element, std::size_t &product) const;
	};

	struct Ddop
	{
		std::shared_ptr<isobus::DeviceDescriptorObjectPool> pool;
		ElementMap map;
		std::string structureLabel; ///< Seven characters derived from the geometry
		std::size_t objectCount = 0;
	};

	/// @brief Builds the DDOP. Throws std::runtime_error if the stack refuses an object.
	Ddop build_ddop(const Profile &profile, const isobus::NAME &clientName, const std::string &softwareVersion);
}
