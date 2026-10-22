#include "model.hpp"

#include <algorithm>
#include <cmath>

namespace sim
{
	Model::Model(const Profile &profile) :
	  prof(profile),
	  delayOnMs(static_cast<std::uint32_t>(profile.delayOn * 1000.0 + 0.5)),
	  delayOffMs(static_cast<std::uint32_t>(profile.delayOff * 1000.0 + 0.5))
	{
		for (const auto &b : prof.booms)
		{
			BoomState s;
			s.sections.resize(b.sections.size());
			for (auto &sec : s.sections)
			{
				sec.sw = prof.switchesDefault;
			}
			booms.push_back(s);
		}
		for (const auto &p : prof.products)
		{
			ProductState s;
			s.operatorRate = p.rate;
			s.tcRate = p.rate;
			s.level = p.bin.level;
			products.push_back(s);
		}
	}

	void Model::set_section_setpoint(std::size_t boom, std::size_t index, bool on)
	{
		booms.at(boom).sections.at(index).setpoint = on;
	}

	void Model::set_switch(std::size_t boom, std::size_t index, bool on)
	{
		booms.at(boom).sections.at(index).sw = on;
	}

	void Model::set_master(std::size_t boom, bool on)
	{
		booms.at(boom).master = on;
	}

	void Model::set_auto(std::size_t boom, bool on)
	{
		booms.at(boom).autoMode = on;
	}

	void Model::set_section_error(std::size_t boom, std::size_t index, bool error)
	{
		booms.at(boom).sections.at(index).error = error;
	}

	void Model::set_prescription(std::size_t product, bool on)
	{
		products.at(product).prescription = on;
	}

	bool Model::set_tc_rate(std::size_t product, double rate)
	{
		const auto &p = prof.products.at(product);
		const double clipped = std::min(std::max(rate, p.min), p.max);
		products.at(product).tcRate = clipped;
		return clipped != rate;
	}

	void Model::set_operator_rate(std::size_t product, double rate)
	{
		const auto &p = prof.products.at(product);
		products.at(product).operatorRate = std::min(std::max(rate, p.min), p.max);
	}

	void Model::set_bin_level(std::size_t product, double level)
	{
		auto &s = products.at(product);
		s.level = std::min(std::max(level, 0.0), prof.products.at(product).bin.capacity);
		s.empty = s.level <= 0;
	}

	void Model::reset_trip_total(std::size_t product)
	{
		products.at(product).tripTotal = 0;
	}

	void Model::reset_boom_totals(std::size_t boom)
	{
		auto &b = booms.at(boom);
		b.areaM2 = 0;
		b.effectiveTimeS = 0;
		b.ineffectiveTimeS = 0;
		b.effectiveDistanceMm = 0;
		b.ineffectiveDistanceMm = 0;
	}

	void Model::set_speed(double mmPerSecond, std::uint32_t nowMs)
	{
		speedMmS = std::max(mmPerSecond, 0.0);
		speedKnown = true;
		speedAtMs = nowMs;
	}

	void Model::set_speed_unknown()
	{
		speedKnown = false;
		speedMmS = 0;
	}

	double Model::target_rate(std::size_t product) const
	{
		const auto &s = products.at(product);
		return s.prescription ? s.tcRate : s.operatorRate;
	}

	void Model::update_commanded(std::size_t boomIndex)
	{
		auto &b = booms[boomIndex];
		for (auto &s : b.sections)
		{
			const bool wanted = b.autoMode ? (s.setpoint && s.sw && b.master) : (s.sw && b.master);
			s.commanded = wanted && !s.error;
		}
	}

	void Model::tick(std::uint32_t nowMs)
	{
		if (!ticked)
		{
			lastTickMs = nowMs;
			ticked = true;
		}
		const double dtS = (nowMs - lastTickMs) / 1000.0;
		lastTickMs = nowMs;

		// Speed goes stale two seconds after the last message (MODEL.md section 6)
		if (speedKnown && (nowMs - speedAtMs) > 2000)
		{
			speedKnown = false;
			speedMmS = 0;
		}
		const double distanceMm = speedKnown ? speedMmS * dtS : 0.0;

		for (std::size_t bi = 0; bi < booms.size(); bi++)
		{
			auto &b = booms[bi];
			update_commanded(bi);
			bool changed = false;
			for (auto &s : b.sections)
			{
				if (s.commanded == s.actual)
				{
					s.pending = false; // a reversal within the delay cancels the change
					continue;
				}
				if (!s.pending)
				{
					s.pending = true;
					s.pendingSince = nowMs;
				}
				const std::uint32_t delay = s.commanded ? delayOnMs : delayOffMs;
				if (nowMs - s.pendingSince >= delay)
				{
					s.actual = s.commanded;
					s.pending = false;
					changed = true;
				}
			}
			if (changed)
			{
				b.generation++;
			}
			// Totals (MODEL.md section 5)
			const std::int32_t widthOn = width_on_mm(bi);
			if (widthOn > 0)
			{
				b.areaM2 += (widthOn / 1000.0) * (distanceMm / 1000.0);
				b.effectiveTimeS += dtS;
				b.effectiveDistanceMm += distanceMm;
			}
			else
			{
				b.ineffectiveTimeS += dtS;
				b.ineffectiveDistanceMm += distanceMm;
			}
		}
		update_products(dtS, distanceMm);
	}

	void Model::update_products(double, double distanceMm)
	{
		for (std::size_t pi = 0; pi < products.size(); pi++)
		{
			auto &s = products[pi];
			const auto &p = prof.products[pi];
			const std::int32_t widthOn = width_on_mm(p.boom);
			const double target = target_rate(pi);
			// Phase 1: the actual follows the target at once when applying (lag, noise and limits are phase 5)
			const bool applying = widthOn > 0 && speedKnown && speedMmS > 0 && !s.empty;
			s.actualRate = applying ? target : 0.0;
			const double areaHa = (widthOn / 1000.0) * (distanceMm / 1000.0) / 10000.0;
			const double applied = s.actualRate * areaHa; // in the product's unit
			s.tripTotal += applied;
			s.lifetimeTotal += applied;
			s.level = std::max(0.0, s.level - applied);
			s.empty = s.level <= 0;
			if (prof.type == "sprayer")
			{
				// flow l/min = rate l/ha × width m × speed km/h / 600; pressure from the flow a nozzle
				const double speedKmh = speedMmS * 3.6 / 1000.0;
				double flow = s.actualRate * (widthOn / 1000.0) * speedKmh / 600.0;
				flow = std::min(flow, prof.sprayer.pumpMaxFlow);
				s.flowLPerMin = flow;
				const double nozzles = std::max(1.0, std::floor(static_cast<double>(widthOn) / prof.sprayer.nozzleSpacing));
				const double perNozzle = flow / nozzles;
				const double ratio = perNozzle / prof.sprayer.nozzleFlow;
				s.pressureBar = widthOn > 0 ? prof.sprayer.nozzlePressure * ratio * ratio : 0.0;
			}
		}
	}

	bool Model::any_actual_on(std::size_t boom) const
	{
		for (const auto &s : booms.at(boom).sections)
		{
			if (s.actual)
			{
				return true;
			}
		}
		return false;
	}

	bool Model::any_actual_on() const
	{
		for (std::size_t i = 0; i < booms.size(); i++)
		{
			if (any_actual_on(i))
			{
				return true;
			}
		}
		return false;
	}

	std::int32_t Model::working_width_mm(std::size_t boom) const
	{
		return prof.booms.at(boom).width();
	}

	std::int32_t Model::width_on_mm(std::size_t boom) const
	{
		std::int32_t w = 0;
		const auto &b = booms.at(boom);
		for (std::size_t i = 0; i < b.sections.size(); i++)
		{
			if (b.sections[i].actual)
			{
				w += prof.booms[boom].sections[i].width;
			}
		}
		return w;
	}

	std::size_t Model::condensed_chunks(std::size_t boom) const
	{
		return (booms.at(boom).sections.size() + 15) / 16;
	}

	namespace
	{
		std::uint32_t condensed(const std::vector<SectionState> &sections, std::size_t chunk, bool actual)
		{
			std::uint32_t value = 0;
			for (std::size_t i = 0; i < 16; i++)
			{
				const std::size_t index = chunk * 16 + i;
				std::uint32_t bits = 0x03; // not installed
				if (index < sections.size())
				{
					const auto &s = sections[index];
					if (actual)
					{
						bits = s.error ? 0x02 : (s.actual ? 0x01 : 0x00);
					}
					else
					{
						bits = s.setpoint ? 0x01 : 0x00;
					}
				}
				value |= bits << (2 * i);
			}
			return value;
		}

		std::string states(const std::vector<SectionState> &sections, int which)
		{
			std::string out;
			for (const auto &s : sections)
			{
				if (which == 0 && s.error)
				{
					out += 'E';
				}
				else
				{
					const bool on = which == 0 ? s.actual : (which == 1 ? s.setpoint : s.commanded);
					out += on ? '1' : '0';
				}
			}
			return out;
		}
	}

	std::uint32_t Model::condensed_actual(std::size_t boom, std::size_t chunk) const
	{
		return condensed(booms.at(boom).sections, chunk, true);
	}

	std::uint32_t Model::condensed_setpoint(std::size_t boom, std::size_t chunk) const
	{
		return condensed(booms.at(boom).sections, chunk, false);
	}

	std::string Model::actual_string(std::size_t boom) const
	{
		return states(booms.at(boom).sections, 0);
	}

	std::string Model::setpoint_string(std::size_t boom) const
	{
		return states(booms.at(boom).sections, 1);
	}

	std::string Model::commanded_string(std::size_t boom) const
	{
		return states(booms.at(boom).sections, 2);
	}

	double Model::row_singulation(std::size_t boom, std::size_t index) const
	{
		const auto &s = booms.at(boom).sections.at(index);
		return s.error ? 0.0 : prof.seeder.singulationBase;
	}

	std::int32_t Model::row_spacing_mm(std::size_t boom, std::size_t index, std::size_t product) const
	{
		// spacing = 1 / (rate per m² × row spacing in m), in mm
		const double perM2 = target_rate(product) / 10000.0;
		const double rowM = prof.booms.at(boom).sections.at(index).width / 1000.0;
		if (perM2 <= 0 || rowM <= 0)
		{
			return 0;
		}
		return static_cast<std::int32_t>(1000.0 / (perM2 * rowM) + 0.5);
	}
}
