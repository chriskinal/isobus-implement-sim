// The behaviour model (docs/MODEL.md): sections with delays, rates, bins, totals, speed.
// No bus: the stack's client reads it through the callbacks in tc_client.cpp, and tests step it.
#pragma once

#include "profile.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sim
{
	struct SectionState
	{
		bool setpoint = false; ///< From the task controller
		bool sw = true; ///< The operator's switch
		bool commanded = false; ///< What the section should be, before the delay
		bool actual = false; ///< What it is
		bool error = false; ///< A faulted section: reports error and is off
		bool pending = false; ///< A change is waiting out its delay
		std::uint32_t pendingSince = 0;
	};

	struct BoomState
	{
		bool master = true; ///< Setpoint work state, DDI 289
		bool autoMode = true; ///< Section control state, DDI 160
		std::vector<SectionState> sections;
		double areaM2 = 0; ///< DDI 116
		double effectiveTimeS = 0; ///< DDI 119
		double ineffectiveTimeS = 0; ///< DDI 120
		double effectiveDistanceMm = 0; ///< DDI 117
		double ineffectiveDistanceMm = 0; ///< DDI 118
		std::uint64_t generation = 0; ///< Bumps when any actual state changes
	};

	struct ProductState
	{
		bool prescription = true; ///< DDI 158: the TC's setpoint rules
		double operatorRate = 0; ///< The operator's target, in the profile's unit
		double tcRate = 0; ///< The task controller's setpoint, clipped
		double actualRate = 0;
		double level = 0; ///< Bin level, in the bin's unit
		double tripTotal = 0;
		double lifetimeTotal = 0;
		double flowLPerMin = 0; ///< Sprayers
		double pressureBar = 0; ///< Sprayers
		bool empty = false;
	};

	class Model
	{
	public:
		explicit Model(const Profile &profile);

		// Inputs from the task controller and the operator
		void set_section_setpoint(std::size_t boom, std::size_t index, bool on);
		void set_switch(std::size_t boom, std::size_t index, bool on);
		void set_master(std::size_t boom, bool on);
		void set_auto(std::size_t boom, bool on);
		void set_section_error(std::size_t boom, std::size_t index, bool error);
		void set_prescription(std::size_t product, bool on);
		/// @returns true if the value was clipped to the product's limits
		bool set_tc_rate(std::size_t product, double rate);
		void set_operator_rate(std::size_t product, double rate);
		void set_bin_level(std::size_t product, double level);
		void reset_trip_total(std::size_t product);
		void reset_boom_totals(std::size_t boom);
		void set_speed(double mmPerSecond, std::uint32_t nowMs);
		void set_speed_unknown();

		/// @brief Advances the model to nowMs. Call every 20 ms.
		void tick(std::uint32_t nowMs);

		// Outputs
		const Profile &profile() const { return prof; }
		const BoomState &boom(std::size_t i) const { return booms.at(i); }
		const SectionState &section(std::size_t boom, std::size_t index) const { return booms.at(boom).sections.at(index); }
		const ProductState &product(std::size_t i) const { return products.at(i); }
		std::size_t boom_count() const { return booms.size(); }
		std::size_t product_count() const { return products.size(); }
		bool any_actual_on(std::size_t boom) const;
		bool any_actual_on() const;
		std::int32_t working_width_mm(std::size_t boom) const; ///< The boom's full width
		std::int32_t width_on_mm(std::size_t boom) const; ///< The width of the sections that are on
		double target_rate(std::size_t product) const;
		bool speed_known() const { return speedKnown; }
		double speed_mm_s() const { return speedMmS; }

		/// @brief Condensed work state, 16 sections a chunk, two bits a section: 00 off, 01 on, 10 error, 11 not installed.
		std::uint32_t condensed_actual(std::size_t boom, std::size_t chunk) const;
		std::uint32_t condensed_setpoint(std::size_t boom, std::size_t chunk) const;
		std::size_t condensed_chunks(std::size_t boom) const;

		/// @brief One character a section: '1' on, '0' off, 'E' error.
		std::string actual_string(std::size_t boom) const;
		std::string setpoint_string(std::size_t boom) const;
		std::string commanded_string(std::size_t boom) const;

		// Seeder rows (static until phase 5 adds the dynamics)
		double row_singulation(std::size_t boom, std::size_t index) const;
		std::int32_t row_spacing_mm(std::size_t boom, std::size_t index, std::size_t product) const;

	private:
		void update_commanded(std::size_t boom);
		void update_products(double dtS, double distanceMm);

		Profile prof;
		std::vector<BoomState> booms;
		std::vector<ProductState> products;
		double speedMmS = 0;
		bool speedKnown = false;
		std::uint32_t speedAtMs = 0;
		std::uint32_t lastTickMs = 0;
		bool ticked = false;
		std::uint32_t delayOnMs;
		std::uint32_t delayOffMs;
	};
}
