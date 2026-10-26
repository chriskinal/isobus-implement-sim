#include "ddop.hpp"
#include "units.hpp"

#include "isobus/isobus/can_constants.hpp"
#include "isobus/isobus/isobus_standard_data_description_indices.hpp"

#include <array>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace sim
{
	bool ElementMap::boom_for(std::uint16_t element, std::size_t &boom) const
	{
		for (std::size_t i = 0; i < booms.size(); i++)
		{
			if (booms[i] == element)
			{
				boom = i;
				return true;
			}
		}
		return false;
	}

	bool ElementMap::section_for(std::uint16_t element, std::size_t &boom, std::size_t &index) const
	{
		for (std::size_t b = 0; b < sections.size(); b++)
		{
			for (std::size_t s = 0; s < sections[b].size(); s++)
			{
				if (sections[b][s] == element)
				{
					boom = b;
					index = s;
					return true;
				}
			}
		}
		return false;
	}

	bool ElementMap::bin_for(std::uint16_t element, std::size_t &product) const
	{
		for (std::size_t i = 0; i < bins.size(); i++)
		{
			if (bins[i] == element)
			{
				product = i;
				return true;
			}
		}
		return false;
	}

	namespace
	{
		using DDI = isobus::DataDescriptionIndex;
		using Element = isobus::task_controller_object::DeviceElementObject;
		using PD = isobus::task_controller_object::DeviceProcessDataObject;

		constexpr std::uint8_t DEFAULT_SET = static_cast<std::uint8_t>(PD::PropertiesBit::MemberOfDefaultSet);
		constexpr std::uint8_t SETTABLE = static_cast<std::uint8_t>(PD::PropertiesBit::Settable);
		constexpr std::uint8_t ON_CHANGE = static_cast<std::uint8_t>(PD::AvailableTriggerMethods::OnChange);
		constexpr std::uint8_t TIME = static_cast<std::uint8_t>(PD::AvailableTriggerMethods::TimeInterval);
		constexpr std::uint8_t TOTAL = static_cast<std::uint8_t>(PD::AvailableTriggerMethods::Total);
		constexpr std::uint16_t NO_PRESENTATION = isobus::NULL_OBJECT_ID;

		std::uint16_t ddi(DDI d)
		{
			return static_cast<std::uint16_t>(d);
		}

		/// Puts objects into the pool, hands out IDs, and links children to their element.
		class Builder
		{
		public:
			explicit Builder(std::shared_ptr<isobus::DeviceDescriptorObjectPool> p) :
			  pool(std::move(p)) {}

			std::uint16_t next_id()
			{
				return nextId++;
			}

			std::uint16_t element(const std::string &name, std::uint16_t number, std::uint16_t parentId, Element::Type type)
			{
				const std::uint16_t id = next_id();
				check(pool->add_device_element(name, number, parentId, type, id), name);
				return id;
			}

			void process_data(std::uint16_t elementId, const std::string &name, std::uint16_t ddiValue, std::uint16_t presentation, std::uint8_t properties, std::uint8_t triggers)
			{
				const std::uint16_t id = next_id();
				check(pool->add_device_process_data(name, ddiValue, presentation, properties, triggers, id), name);
				link(elementId, id);
			}

			void property(std::uint16_t elementId, const std::string &name, std::int32_t value, std::uint16_t ddiValue, std::uint16_t presentation)
			{
				const std::uint16_t id = next_id();
				check(pool->add_device_property(name, value, ddiValue, presentation, id), name);
				link(elementId, id);
			}

			/// A presentation object, made once per unit.
			std::uint16_t presentation(const std::string &unit, float scale, std::uint8_t decimals)
			{
				auto found = presentations.find(unit);
				if (found != presentations.end())
				{
					return found->second;
				}
				const std::uint16_t id = next_id();
				check(pool->add_device_value_presentation(unit, 0, scale, decimals, id), unit);
				presentations[unit] = id;
				return id;
			}

			std::size_t count() const
			{
				return nextId;
			}

		private:
			void link(std::uint16_t elementId, std::uint16_t childId)
			{
				auto object = std::static_pointer_cast<Element>(pool->get_object_by_id(elementId));
				if (!object)
				{
					throw std::runtime_error("ddop: no element " + std::to_string(elementId));
				}
				object->add_reference_to_child_object(childId);
			}

			void check(bool ok, const std::string &what)
			{
				if (!ok)
				{
					throw std::runtime_error("ddop: the stack refused \"" + what + "\"");
				}
			}

			std::shared_ptr<isobus::DeviceDescriptorObjectPool> pool;
			std::uint16_t nextId = 1; // 0 is the device object
			std::map<std::string, std::uint16_t> presentations;
		};

		/// Seven characters from a hash of everything that shapes the DDOP, so a changed
		/// profile never matches a structure the task controller stored.
		std::string structure_label(const Profile &p)
		{
			std::string canon = p.type + "|" + to_string(p.sectionControl) + "|";
			for (const auto &b : p.booms)
			{
				canon += std::to_string(b.offsetX) + "," + std::to_string(b.offsetY) + "," + std::to_string(b.offsetZ) + ":";
				for (const auto &s : b.sections)
				{
					canon += std::to_string(s.width) + "/" + std::to_string(s.offsetX) + "/" + std::to_string(s.offsetY) + ";";
				}
				canon += "|";
			}
			for (const auto &pr : p.products)
			{
				canon += pr.unit + "," + pr.bin.unit + "," + pr.presentation + "," + std::to_string(pr.boom) + "|";
			}
			canon += std::to_string(p.connector.type) + "," + std::to_string(p.connector.x) + "," + std::to_string(p.connector.y);
			canon += p.seeder.rowData ? "|rows" : "";
			for (auto n : p.elementNumbers)
			{
				canon += "," + std::to_string(n);
			}
			std::uint32_t h = 2166136261u;
			for (unsigned char c : canon)
			{
				h ^= c;
				h *= 16777619u;
			}
			char out[8];
			std::snprintf(out, sizeof(out), "I%06X", h & 0xFFFFFF);
			return out;
		}

		struct RateDdis
		{
			DDI setpoint, actual, def, min, max, capacity, content, tripTotal, lifetimeTotal;
		};

		RateDdis rate_ddis(const std::string &unit)
		{
			if (unit == "l/ha")
			{
				return { DDI::SetpointVolumePerAreaApplicationRate, DDI::ActualVolumePerAreaApplicationRate, DDI::DefaultVolumePerAreaApplicationRate, DDI::MinimumVolumePerAreaApplicationRate, DDI::MaximumVolumePerAreaApplicationRate, DDI::MaximumVolumeContent, DDI::ActualVolumeContent, DDI::ApplicationTotalVolume_L, DDI::LifetimeApplicationTotalVolume };
			}
			if (unit == "kg/ha")
			{
				return { DDI::SetpointMassPerAreaApplicationRate, DDI::ActualMassPerAreaApplicationRate, DDI::DefaultMassPerAreaApplicationRate, DDI::MinimumMassPerAreaApplicationRate, DDI::MaximumMassPerAreaApplicationRate, DDI::MaximumMassContent, DDI::ActualMassContent, DDI::ApplicationTotalMass_kg, DDI::LifetimeApplicationTotalMass };
			}
			return { DDI::SetpointCountPerAreaApplicationRate, DDI::ActualCountPerAreaApplicationRate, DDI::DefaultCountPerAreaApplicationRate, DDI::MinimumCountPerAreaApplicationRate, DDI::MaximumCountPerAreaApplicationRate, DDI::MaximumCountContent, DDI::ActualCountContent, DDI::ApplicationTotalCount, DDI::LifetimeApplicationTotalCount };
		}

		/// The presentation for a rate: the bus unit shown in the profile's unit, or in gal/ac.
		std::uint16_t rate_presentation(Builder &b, const Product &p)
		{
			if (p.presentation == "gal/ac" && p.unit == "l/ha")
			{
				return b.presentation("gal/ac", 1.0f / (9.35396f * 10000.0f), 1);
			}
			if (p.unit == "l/ha")
			{
				return b.presentation("l/ha", 0.0001f, 1);
			}
			if (p.unit == "kg/ha")
			{
				return b.presentation("kg/ha", 0.01f, 1);
			}
			return b.presentation("seeds/ha", 10.0f, 0);
		}

		std::uint16_t content_presentation(Builder &b, const Product &p)
		{
			if (p.presentation == "gal/ac" && p.bin.unit == "l")
			{
				return b.presentation("gal", 1.0f / 3785.41f, 0);
			}
			if (p.bin.unit == "l")
			{
				return b.presentation("l", 0.001f, 0);
			}
			if (p.bin.unit == "kg")
			{
				return b.presentation("kg", 0.001f, 1);
			}
			return b.presentation("seeds", 1.0f, 0);
		}

		std::uint16_t total_presentation(Builder &b, const Product &p)
		{
			if (p.presentation == "gal/ac" && p.bin.unit == "l")
			{
				return b.presentation("gal total", 1.0f / 3.78541f, 0);
			}
			return b.presentation(p.bin.unit + " total", 1.0f, 0);
		}
	}

	Ddop build_ddop(const Profile &profile, const isobus::NAME &clientName, const std::string &softwareVersion)
	{
		Ddop out;
		out.pool = std::make_shared<isobus::DeviceDescriptorObjectPool>();
		out.pool->clear();
		out.structureLabel = structure_label(profile);
		Builder b(out.pool);

		// English, decimal point, metric everywhere
		const std::array<std::uint8_t, 7> localization = { 'e', 'n', 0x50, 0x00, 0x00, 0x00, 0xFF };
		if (!out.pool->add_device(profile.name.substr(0, 32), softwareVersion, std::to_string(profile.identity), out.structureLabel, localization, std::vector<std::uint8_t>(), clientName.get_full_name()))
		{
			throw std::runtime_error("ddop: the stack refused the device object");
		}

		const std::uint16_t mm = b.presentation("mm", 1.0f, 0);
		const std::uint16_t metres = b.presentation("m", 0.001f, 2);
		const std::uint16_t m2 = b.presentation("m^2", 1.0f, 0);
		const std::uint16_t seconds = b.presentation("s", 1.0f, 0);

		// Element numbers in build order: the profile's list, or 0, 1, 2 and on
		std::size_t built = 0;
		auto next_element = [&]() -> std::uint16_t {
			const std::size_t i = built++;
			return profile.elementNumbers.empty() ? static_cast<std::uint16_t>(i) : profile.elementNumbers.at(i);
		};

		// The device element
		const char *deviceName = profile.type == "sprayer" ? "Sprayer" : (profile.type == "spreader" ? "Spreader" : "Seeder");
		out.map.device = next_element();
		const std::uint16_t deviceEl = b.element(deviceName, out.map.device, 0, Element::Type::Device);
		b.process_data(deviceEl, "Actual Work State", ddi(DDI::ActualWorkState), NO_PRESENTATION, DEFAULT_SET, ON_CHANGE);
		b.process_data(deviceEl, "Request Default PD", ddi(DDI::RequestDefaultProcessData), NO_PRESENTATION, 0, TOTAL);

		// The connector
		out.map.connector = next_element();
		const std::uint16_t connectorEl = b.element("Connector", out.map.connector, deviceEl, Element::Type::Connector);
		b.process_data(connectorEl, "Connector X", ddi(DDI::DeviceElementOffsetX), mm, SETTABLE, 0);
		b.process_data(connectorEl, "Connector Y", ddi(DDI::DeviceElementOffsetY), mm, SETTABLE, 0);
		b.property(connectorEl, "Connector Type", profile.connector.type, ddi(DDI::ConnectorType), NO_PRESENTATION);

		// Booms and their sections
		for (std::size_t bi = 0; bi < profile.booms.size(); bi++)
		{
			const auto &boom = profile.booms[bi];
			const std::string boomName = profile.booms.size() == 1 ? "Boom" : "Boom " + std::to_string(bi + 1);
			out.map.booms.push_back(next_element());
			const std::uint16_t boomEl = b.element(boomName, out.map.booms.back(), deviceEl, Element::Type::Function);
			b.property(boomEl, "Offset X", boom.offsetX, ddi(DDI::DeviceElementOffsetX), mm);
			b.property(boomEl, "Offset Y", boom.offsetY, ddi(DDI::DeviceElementOffsetY), mm);
			b.property(boomEl, "Offset Z", boom.offsetZ, ddi(DDI::DeviceElementOffsetZ), mm);
			b.process_data(boomEl, "Working Width", ddi(DDI::ActualWorkingWidth), metres, DEFAULT_SET, ON_CHANGE);
			b.process_data(boomEl, "Actual Work State", ddi(DDI::ActualWorkState), NO_PRESENTATION, DEFAULT_SET, ON_CHANGE);
			b.process_data(boomEl, "Setpoint Work State", ddi(DDI::SetpointWorkState), NO_PRESENTATION, DEFAULT_SET | SETTABLE, ON_CHANGE);
			b.process_data(boomEl, "Section Control State", ddi(DDI::SectionControlState), NO_PRESENTATION, DEFAULT_SET | SETTABLE, ON_CHANGE | TIME);
			b.process_data(boomEl, "Total Area", ddi(DDI::TotalArea), m2, DEFAULT_SET | SETTABLE, TOTAL);
			b.process_data(boomEl, "Effective Distance", ddi(DDI::EffectiveTotalDistance), metres, DEFAULT_SET | SETTABLE, TOTAL);
			b.process_data(boomEl, "Ineffective Distance", ddi(DDI::IneffectiveTotalDistance), metres, DEFAULT_SET | SETTABLE, TOTAL);
			b.process_data(boomEl, "Effective Time", ddi(DDI::EffectiveTotalTime), seconds, DEFAULT_SET | SETTABLE, TOTAL);
			b.process_data(boomEl, "Ineffective Time", ddi(DDI::IneffectiveTotalTime), seconds, DEFAULT_SET | SETTABLE, TOTAL);

			const std::size_t chunks = (boom.sections.size() + 15) / 16;
			for (std::size_t c = 0; c < chunks; c++)
			{
				const std::string range = " " + std::to_string(16 * c + 1) + "-" + std::to_string(16 * c + 16);
				switch (profile.sectionControl)
				{
					case SectionControl::Setpoint:
						b.process_data(boomEl, "Actual Work State" + range, ddi(DDI::ActualCondensedWorkState1_16) + static_cast<std::uint16_t>(c), NO_PRESENTATION, DEFAULT_SET, ON_CHANGE);
						b.process_data(boomEl, "Setpoint Work State" + range, ddi(DDI::SetpointCondensedWorkState1_16) + static_cast<std::uint16_t>(c), NO_PRESENTATION, DEFAULT_SET | SETTABLE, ON_CHANGE);
						break;
					case SectionControl::ActualSettable:
						b.process_data(boomEl, "Actual Work State" + range, ddi(DDI::ActualCondensedWorkState1_16) + static_cast<std::uint16_t>(c), NO_PRESENTATION, DEFAULT_SET | SETTABLE, ON_CHANGE);
						break;
					case SectionControl::PerElement:
						break;
				}
			}

			out.map.sections.emplace_back();
			for (std::size_t si = 0; si < boom.sections.size(); si++)
			{
				const auto &s = boom.sections[si];
				out.map.sections.back().push_back(next_element());
				const std::uint16_t sectionEl = b.element(s.name.substr(0, 32), out.map.sections.back().back(), boomEl, Element::Type::Section);
				// Section offsets are from the device reference point: the boom's plus the section's
				b.property(sectionEl, "Offset X", boom.offsetX + s.offsetX, ddi(DDI::DeviceElementOffsetX), mm);
				b.property(sectionEl, "Offset Y", boom.offsetY + s.offsetY, ddi(DDI::DeviceElementOffsetY), mm);
				b.property(sectionEl, "Width", s.width, ddi(DDI::ActualWorkingWidth), mm);
				const bool rowData = profile.type == "seeder" && profile.seeder.rowData;
				if (profile.sectionControl == SectionControl::PerElement)
				{
					b.process_data(sectionEl, "Work State", ddi(DDI::ActualWorkState), NO_PRESENTATION, DEFAULT_SET | SETTABLE, ON_CHANGE);
				}
				else if (rowData)
				{
					b.process_data(sectionEl, "Work State", ddi(DDI::ActualWorkState), NO_PRESENTATION, DEFAULT_SET, ON_CHANGE);
				}
				if (rowData)
				{
					const std::uint16_t percent = b.presentation("%", 0.0001f, 1);
					b.process_data(sectionEl, "Seed Spacing", ddi(DDI::ActualSpacingApplicationRate), mm, DEFAULT_SET, ON_CHANGE | TIME);
					b.process_data(sectionEl, "Singulation", ddi(DDI::ActualSeedSingulationPercentage), percent, DEFAULT_SET, ON_CHANGE | TIME);
					b.process_data(sectionEl, "Skips", ddi(DDI::ActualSeedSkipPercentage), percent, DEFAULT_SET, ON_CHANGE | TIME);
					b.process_data(sectionEl, "Multiples", ddi(DDI::ActualSeedMultiplePercentage), percent, DEFAULT_SET, ON_CHANGE | TIME);
					b.process_data(sectionEl, "Avg Singulation", ddi(DDI::AverageSeedSingulationPercentage), percent, DEFAULT_SET, TOTAL);
					b.process_data(sectionEl, "Avg Skips", ddi(DDI::AverageSeedSkipPercentage), percent, DEFAULT_SET, TOTAL);
					b.process_data(sectionEl, "Avg Multiples", ddi(DDI::AverageSeedMultiplePercentage), percent, DEFAULT_SET, TOTAL);
				}
			}
		}

		// Products: one bin each
		for (std::size_t pi = 0; pi < profile.products.size(); pi++)
		{
			const auto &p = profile.products[pi];
			const RateDdis d = rate_ddis(p.unit);
			out.map.bins.push_back(next_element());
			const std::uint16_t binEl = b.element(p.name.substr(0, 32), out.map.bins.back(), deviceEl, Element::Type::Bin);
			const std::uint16_t ratePres = rate_presentation(b, p);
			const std::uint16_t contentPres = content_presentation(b, p);
			const std::uint16_t totalPres = total_presentation(b, p);
			const double rs = rate_scale(p.unit);
			b.process_data(binEl, "Setpoint Rate", ddi(d.setpoint), ratePres, DEFAULT_SET | SETTABLE, ON_CHANGE);
			b.process_data(binEl, "Actual Rate", ddi(d.actual), ratePres, DEFAULT_SET, ON_CHANGE | TIME);
			b.property(binEl, "Default Rate", to_ddi(p.rate, rs), ddi(d.def), ratePres);
			b.property(binEl, "Minimum Rate", to_ddi(p.min, rs), ddi(d.min), ratePres);
			b.property(binEl, "Maximum Rate", to_ddi(p.max, rs), ddi(d.max), ratePres);
			b.process_data(binEl, "Capacity", ddi(d.capacity), contentPres, DEFAULT_SET, ON_CHANGE | TIME);
			b.process_data(binEl, "Content", ddi(d.content), contentPres, DEFAULT_SET | SETTABLE, ON_CHANGE | TIME);
			b.process_data(binEl, "Applied Total", ddi(d.tripTotal), totalPres, DEFAULT_SET | SETTABLE, TOTAL);
			b.process_data(binEl, "Lifetime Total", ddi(d.lifetimeTotal), totalPres, DEFAULT_SET, TOTAL);
			b.process_data(binEl, "Rx Control State", ddi(DDI::PrescriptionControlState), NO_PRESENTATION, DEFAULT_SET | SETTABLE, ON_CHANGE | TIME);
			// DDI 179: 1 fertilizing, 2 sowing and planting, 3 crop protection
			b.property(binEl, "Operation Type", profile.type == "sprayer" ? 3 : (profile.type == "spreader" ? 1 : 2), ddi(DDI::ActualCulturalPractice), NO_PRESENTATION);
			if (profile.type == "sprayer")
			{
				const std::uint16_t bar = b.presentation("bar", 0.00001f, 2);
				const std::uint16_t lpm = b.presentation("l/min", 0.00006f, 1);
				b.process_data(binEl, "Flow", ddi(DDI::ActualVolumePerTimeApplicationRate), lpm, DEFAULT_SET, ON_CHANGE | TIME);
				b.process_data(binEl, "Setpoint Pressure", ddi(DDI::SetpointProductPressure), bar, DEFAULT_SET | SETTABLE, ON_CHANGE);
				b.process_data(binEl, "Pressure", ddi(DDI::ActualProductPressure), bar, DEFAULT_SET, ON_CHANGE | TIME);
			}
			else if (profile.type == "seeder" && profile.seeder.kind == "planter")
			{
				b.process_data(binEl, "Setpoint Depth", ddi(DDI::SetpointSeedingDepth), mm, DEFAULT_SET | SETTABLE, ON_CHANGE);
				b.process_data(binEl, "Actual Depth", ddi(DDI::ActualSeedingDepth), mm, DEFAULT_SET, ON_CHANGE | TIME);
			}
		}
		out.objectCount = b.count();
		return out;
	}
}
