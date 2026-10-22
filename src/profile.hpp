// A machine profile (docs/MODEL.md section 10): the tree the DDOP and the behaviour are built from.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim
{
	struct Section
	{
		std::string name;
		std::int32_t width = 0; ///< mm
		std::int32_t offsetX = 0; ///< mm, forward positive
		std::int32_t offsetY = 0; ///< mm, right positive; the section's centre
	};

	struct Boom
	{
		std::int32_t offsetX = 0;
		std::int32_t offsetY = 0;
		std::int32_t offsetZ = 0;
		std::vector<Section> sections;
		std::int32_t width() const;
	};

	struct Bin
	{
		std::string unit; ///< "l", "kg" or "seeds"
		double capacity = 0;
		double level = 0;
		double low = 0.05; ///< fraction of capacity for the low alarm
	};

	struct Product
	{
		std::string name;
		std::string unit; ///< "l/ha", "kg/ha" or "seeds/ha"
		std::string presentation; ///< "" for the unit itself, or "gal/ac"
		double rate = 0;
		double min = 0;
		double max = 0;
		double tau = 1.0;
		double noise = 0.0;
		std::size_t boom = 0;
		Bin bin;
	};

	enum class SectionControl
	{
		Setpoint, ///< 290+ settable, 289 master, 161+ read-only (default)
		ActualSettable, ///< 161+ settable, no 290
		PerElement ///< 141 settable on every section element, no condensed objects
	};

	struct Connector
	{
		std::uint8_t type = 1; ///< DDI 157: 1 drawbar, 3 three-point mounted
		std::int32_t x = 0;
		std::int32_t y = 0;
	};

	struct SeederSettings
	{
		std::string kind = "planter"; ///< "planter" or "drill"
		bool rowData = true;
		double meterMaxSpeed = 16; ///< km/h
		double singulationBase = 0.985;
		double singulationKnee = 10;
		double singulationSlope = 0.01;
		double singulationWalk = 0.003;
		std::int32_t depthSetpoint = 50;
		std::int32_t depthMin = 20;
		std::int32_t depthMax = 90;
	};

	struct SprayerSettings
	{
		std::int32_t nozzleSpacing = 500; ///< mm
		double nozzleFlow = 1.6; ///< l/min at nozzlePressure
		double nozzlePressure = 3.0; ///< bar
		double pumpMaxFlow = 200; ///< l/min
		double pressureMin = 1.0;
		double pressureMax = 6.0;
		double rateStep = 0.1;
	};

	struct Profile
	{
		std::string name;
		std::string type; ///< "seeder" or "sprayer"
		std::uint32_t identity = 1;
		Connector connector;
		SectionControl sectionControl = SectionControl::Setpoint;
		double delayOn = 0.3; ///< s
		double delayOff = 0.2; ///< s
		bool switchesDefault = true;
		std::vector<Boom> booms;
		std::vector<Product> products;
		std::string speedSource = "bus";
		double alarmNoSpeed = 10;
		double alarmNoTaskController = 30;
		std::string pool;
		SeederSettings seeder;
		SprayerSettings sprayer;

		std::size_t total_sections() const;
	};

	/// @brief Reads and checks a profile file. Throws std::runtime_error with the reason.
	Profile load_profile(const std::string &path);

	/// @brief Parses a profile from JSON text. Throws std::runtime_error with the reason.
	Profile parse_profile(const std::string &jsonText);

	const char *to_string(SectionControl mode);
	bool section_control_from_string(const std::string &text, SectionControl &out);
}
