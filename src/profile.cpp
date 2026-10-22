#include "profile.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sim
{
	using json = nlohmann::json;

	std::int32_t Boom::width() const
	{
		std::int32_t total = 0;
		for (const auto &s : sections)
		{
			total += s.width;
		}
		return total;
	}

	std::size_t Profile::total_sections() const
	{
		std::size_t n = 0;
		for (const auto &b : booms)
		{
			n += b.sections.size();
		}
		return n;
	}

	const char *to_string(SectionControl mode)
	{
		switch (mode)
		{
			case SectionControl::Setpoint:
				return "setpoint";
			case SectionControl::ActualSettable:
				return "actualSettable";
			case SectionControl::PerElement:
				return "perElement";
		}
		return "?";
	}

	bool section_control_from_string(const std::string &text, SectionControl &out)
	{
		if (text == "setpoint")
		{
			out = SectionControl::Setpoint;
		}
		else if (text == "actualSettable")
		{
			out = SectionControl::ActualSettable;
		}
		else if (text == "perElement")
		{
			out = SectionControl::PerElement;
		}
		else
		{
			return false;
		}
		return true;
	}

	namespace
	{
		[[noreturn]] void fail(const std::string &what)
		{
			throw std::runtime_error("profile: " + what);
		}

		template<typename T>
		T get(const json &j, const char *key, T fallback)
		{
			return j.contains(key) ? j.at(key).get<T>() : fallback;
		}

		template<typename T>
		T need(const json &j, const char *key, const std::string &where)
		{
			if (!j.contains(key))
			{
				fail(where + " needs \"" + key + "\"");
			}
			return j.at(key).get<T>();
		}

		/// Equal sections of one width, centred on the boom, left to right.
		std::vector<Section> equal_sections(int count, std::int32_t width, std::int32_t offsetX, const std::string &prefix)
		{
			if (count < 1 || count > 256)
			{
				fail("section count must be 1 to 256");
			}
			if (width <= 0)
			{
				fail("section width must be positive");
			}
			std::vector<Section> out;
			const std::int32_t total = width * count;
			for (int i = 0; i < count; i++)
			{
				Section s;
				s.name = prefix + " " + std::to_string(i + 1);
				s.width = width;
				s.offsetX = offsetX;
				s.offsetY = -total / 2 + i * width + width / 2;
				out.push_back(s);
			}
			return out;
		}

		Boom parse_boom(const json &j, std::size_t index)
		{
			const std::string where = "booms[" + std::to_string(index) + "]";
			Boom b;
			b.offsetX = get<std::int32_t>(j, "offsetX", 0);
			b.offsetY = get<std::int32_t>(j, "offsetY", 0);
			b.offsetZ = get<std::int32_t>(j, "offsetZ", 0);
			if (j.contains("rows"))
			{
				const auto &r = j.at("rows");
				b.sections = equal_sections(need<int>(r, "count", where + ".rows"), need<std::int32_t>(r, "spacing", where + ".rows"), get<std::int32_t>(r, "offsetX", 0), "Row");
			}
			else if (j.contains("sections") && j.at("sections").is_object())
			{
				const auto &s = j.at("sections");
				b.sections = equal_sections(need<int>(s, "count", where + ".sections"), need<std::int32_t>(s, "width", where + ".sections"), get<std::int32_t>(s, "offsetX", 0), "Section");
			}
			else if (j.contains("sections") && j.at("sections").is_array())
			{
				std::size_t i = 0;
				for (const auto &sj : j.at("sections"))
				{
					Section s;
					s.name = get<std::string>(sj, "name", "Section " + std::to_string(i + 1));
					s.width = need<std::int32_t>(sj, "width", where + ".sections[" + std::to_string(i) + "]");
					s.offsetX = get<std::int32_t>(sj, "offsetX", 0);
					s.offsetY = need<std::int32_t>(sj, "offsetY", where + ".sections[" + std::to_string(i) + "]");
					if (s.width <= 0)
					{
						fail(where + ": section widths must be positive");
					}
					b.sections.push_back(s);
					i++;
				}
			}
			if (b.sections.empty())
			{
				fail(where + " needs \"sections\" or \"rows\"");
			}
			return b;
		}

		Product parse_product(const json &j, std::size_t index, std::size_t boomCount)
		{
			const std::string where = "products[" + std::to_string(index) + "]";
			Product p;
			p.name = get<std::string>(j, "name", "Product " + std::to_string(index + 1));
			p.unit = need<std::string>(j, "unit", where);
			if (p.unit != "l/ha" && p.unit != "kg/ha" && p.unit != "seeds/ha")
			{
				fail(where + ": unit must be l/ha, kg/ha or seeds/ha");
			}
			p.presentation = get<std::string>(j, "presentation", "");
			p.rate = need<double>(j, "rate", where);
			p.min = get<double>(j, "min", 0);
			p.max = get<double>(j, "max", p.rate * 4);
			p.tau = get<double>(j, "tau", 1.0);
			p.noise = get<double>(j, "noise", 0.0);
			p.boom = get<std::size_t>(j, "boom", 0);
			if (p.boom >= boomCount)
			{
				fail(where + ": boom index out of range");
			}
			if (j.contains("bin"))
			{
				const auto &b = j.at("bin");
				p.bin.unit = get<std::string>(b, "unit", p.unit == "l/ha" ? "l" : (p.unit == "kg/ha" ? "kg" : "seeds"));
				p.bin.capacity = need<double>(b, "capacity", where + ".bin");
				p.bin.level = get<double>(b, "level", p.bin.capacity);
				p.bin.low = get<double>(b, "low", 0.05);
			}
			else
			{
				p.bin.unit = p.unit == "l/ha" ? "l" : (p.unit == "kg/ha" ? "kg" : "seeds");
			}
			if (p.bin.unit != "l" && p.bin.unit != "kg" && p.bin.unit != "seeds")
			{
				fail(where + ".bin: unit must be l, kg or seeds");
			}
			return p;
		}
	}

	Profile parse_profile(const std::string &jsonText)
	{
		json j;
		try
		{
			j = json::parse(jsonText);
		}
		catch (const json::exception &e)
		{
			fail(std::string("not valid JSON: ") + e.what());
		}
		Profile p;
		p.name = need<std::string>(j, "name", "profile");
		p.type = need<std::string>(j, "type", "profile");
		if (p.type != "seeder" && p.type != "sprayer")
		{
			fail("type must be seeder or sprayer");
		}
		p.identity = get<std::uint32_t>(j, "identity", 1);
		if (j.contains("connector"))
		{
			const auto &c = j.at("connector");
			if (c.contains("type") && c.at("type").is_string())
			{
				const auto t = c.at("type").get<std::string>();
				if (t == "drawbar")
				{
					p.connector.type = 1;
				}
				else if (t == "threePoint")
				{
					p.connector.type = 3;
				}
				else
				{
					fail("connector.type must be drawbar, threePoint or a number");
				}
			}
			else
			{
				p.connector.type = get<std::uint8_t>(c, "type", 1);
			}
			p.connector.x = get<std::int32_t>(c, "x", 0);
			p.connector.y = get<std::int32_t>(c, "y", 0);
		}
		if (!section_control_from_string(get<std::string>(j, "sectionControl", "setpoint"), p.sectionControl))
		{
			fail("sectionControl must be setpoint, actualSettable or perElement");
		}
		if (j.contains("sectionDelay"))
		{
			p.delayOn = get<double>(j.at("sectionDelay"), "on", p.delayOn);
			p.delayOff = get<double>(j.at("sectionDelay"), "off", p.delayOff);
		}
		p.switchesDefault = get<std::string>(j, "switchesDefault", "on") != "off";
		if (!j.contains("booms") || !j.at("booms").is_array() || j.at("booms").empty())
		{
			fail("needs at least one boom");
		}
		std::size_t i = 0;
		for (const auto &bj : j.at("booms"))
		{
			p.booms.push_back(parse_boom(bj, i++));
		}
		if (p.total_sections() > 256)
		{
			fail("more than 256 sections in all");
		}
		if (j.contains("products"))
		{
			i = 0;
			for (const auto &pj : j.at("products"))
			{
				p.products.push_back(parse_product(pj, i++, p.booms.size()));
			}
		}
		if (j.contains("speed"))
		{
			p.speedSource = get<std::string>(j.at("speed"), "source", "bus");
		}
		if (j.contains("alarms"))
		{
			p.alarmNoSpeed = get<double>(j.at("alarms"), "noSpeed", p.alarmNoSpeed);
			p.alarmNoTaskController = get<double>(j.at("alarms"), "noTaskController", p.alarmNoTaskController);
		}
		p.pool = get<std::string>(j, "pool", "");
		if (j.contains("seeder"))
		{
			const auto &s = j.at("seeder");
			p.seeder.kind = get<std::string>(s, "kind", "planter");
			p.seeder.rowData = get<bool>(s, "rowData", p.seeder.kind == "planter");
			if (s.contains("meter"))
			{
				p.seeder.meterMaxSpeed = get<double>(s.at("meter"), "maxSpeed", p.seeder.meterMaxSpeed);
			}
			if (s.contains("singulation"))
			{
				const auto &g = s.at("singulation");
				p.seeder.singulationBase = get<double>(g, "base", p.seeder.singulationBase);
				p.seeder.singulationKnee = get<double>(g, "knee", p.seeder.singulationKnee);
				p.seeder.singulationSlope = get<double>(g, "slope", p.seeder.singulationSlope);
				p.seeder.singulationWalk = get<double>(g, "walk", p.seeder.singulationWalk);
			}
			if (s.contains("depth"))
			{
				const auto &d = s.at("depth");
				p.seeder.depthSetpoint = get<std::int32_t>(d, "setpoint", p.seeder.depthSetpoint);
				p.seeder.depthMin = get<std::int32_t>(d, "min", p.seeder.depthMin);
				p.seeder.depthMax = get<std::int32_t>(d, "max", p.seeder.depthMax);
			}
		}
		else if (p.type == "seeder")
		{
			p.seeder.rowData = false;
		}
		if (j.contains("sprayer"))
		{
			const auto &s = j.at("sprayer");
			if (s.contains("nozzle"))
			{
				const auto &n = s.at("nozzle");
				p.sprayer.nozzleSpacing = get<std::int32_t>(n, "spacing", p.sprayer.nozzleSpacing);
				p.sprayer.nozzleFlow = get<double>(n, "flow", p.sprayer.nozzleFlow);
				p.sprayer.nozzlePressure = get<double>(n, "pressure", p.sprayer.nozzlePressure);
			}
			if (s.contains("pump"))
			{
				p.sprayer.pumpMaxFlow = get<double>(s.at("pump"), "maxFlow", p.sprayer.pumpMaxFlow);
			}
			if (s.contains("pressure"))
			{
				p.sprayer.pressureMin = get<double>(s.at("pressure"), "min", p.sprayer.pressureMin);
				p.sprayer.pressureMax = get<double>(s.at("pressure"), "max", p.sprayer.pressureMax);
			}
			p.sprayer.rateStep = get<double>(s, "rateStep", p.sprayer.rateStep);
		}
		return p;
	}

	Profile load_profile(const std::string &path)
	{
		std::ifstream in(path);
		if (!in)
		{
			fail("cannot read " + path);
		}
		std::stringstream buffer;
		buffer << in.rdbuf();
		return parse_profile(buffer.str());
	}
}
