// The model without a bus (docs/MODEL.md section 9), and the DDOP built from each shipped profile.
//
//     test_model <profiles directory>
#include "../src/ddop.hpp"
#include "../src/model.hpp"
#include "../src/profile.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;
static int argc_copy = 0;
static char **argv_copy = nullptr;

#define CHECK(cond, ...)                                             \
	do                                                                 \
	{                                                                  \
		checks++;                                                        \
		if (!(cond))                                                     \
		{                                                                \
			failures++;                                                    \
			std::printf("FAIL  %s:%d  ", __FILE__, __LINE__);               \
			std::printf(__VA_ARGS__);                                      \
			std::printf("\n");                                             \
		}                                                                \
	} while (0)

static bool near(double a, double b, double tolerance)
{
	return std::fabs(a - b) <= tolerance;
}

static void test_profiles(const std::string &dir)
{
	auto p12 = sim::load_profile(dir + "/planter-12.json");
	CHECK(p12.total_sections() == 12, "planter-12 sections %zu", p12.total_sections());
	CHECK(p12.booms[0].width() == 9144, "planter-12 width %d", p12.booms[0].width());
	CHECK(p12.booms[0].sections[0].offsetY == -4191, "planter-12 row 1 Y %d", p12.booms[0].sections[0].offsetY);
	CHECK(p12.booms[0].sections[11].offsetY == 4191, "planter-12 row 12 Y %d", p12.booms[0].sections[11].offsetY);
	CHECK(p12.products.size() == 1 && p12.products[0].unit == "seeds/ha", "planter-12 product");
	CHECK(p12.connector.type == 3, "planter-12 connector %d", p12.connector.type);
	CHECK(p12.seeder.rowData, "planter-12 row data");

	auto p48 = sim::load_profile(dir + "/planter-48.json");
	CHECK(p48.total_sections() == 48 && p48.booms[0].width() == 18288, "planter-48 %zu sections, %d mm", p48.total_sections(), p48.booms[0].width());

	auto drill = sim::load_profile(dir + "/drill-6m.json");
	CHECK(drill.products.size() == 2 && drill.products[1].bin.unit == "kg", "drill-6m products");
	CHECK(drill.booms[0].width() == 6000, "drill-6m width %d", drill.booms[0].width());
	CHECK(!drill.seeder.rowData, "drill-6m has no row data");

	auto air = sim::load_profile(dir + "/airseeder-12m.json");
	CHECK(air.booms.size() == 2 && air.products[1].boom == 1, "airseeder-12m two booms");
	CHECK(air.booms[0].width() == 12000 && air.booms[1].width() == 12000, "airseeder-12m boom widths");

	auto s24 = sim::load_profile(dir + "/sprayer-24.json");
	CHECK(s24.total_sections() == 12 && s24.booms[0].width() == 24000, "sprayer-24 geometry");
	CHECK(s24.booms[0].sections[0].offsetY == -11000 && s24.booms[0].sections[11].offsetY == 11000, "sprayer-24 outer sections %d %d", s24.booms[0].sections[0].offsetY, s24.booms[0].sections[11].offsetY);
	CHECK(s24.connector.type == 1 && s24.booms[0].offsetX == -4500, "sprayer-24 connector and boom offset");

	auto s120 = sim::load_profile(dir + "/sprayer-120ft.json");
	CHECK(s120.total_sections() == 7 && s120.booms[0].width() == 36576, "sprayer-120ft %zu sections, %d mm", s120.total_sections(), s120.booms[0].width());
	CHECK(s120.products[0].presentation == "gal/ac", "sprayer-120ft presentation");

	auto s36 = sim::load_profile(dir + "/sprayer-36-nozzle.json");
	CHECK(s36.total_sections() == 72, "sprayer-36-nozzle sections %zu", s36.total_sections());

	auto s12 = sim::load_profile(dir + "/sprayer-mounted-12.json");
	CHECK(s12.total_sections() == 5 && s12.connector.type == 3, "sprayer-mounted-12");

	// Overrides and errors
	bool threw = false;
	try
	{
		sim::parse_profile("{\"name\":\"x\",\"type\":\"sprayer\"}");
	}
	catch (const std::runtime_error &)
	{
		threw = true;
	}
	CHECK(threw, "a profile without booms is refused");
	threw = false;
	try
	{
		sim::parse_profile("{\"name\":\"x\",\"type\":\"sprayer\",\"sectionControl\":\"bogus\",\"booms\":[{\"sections\":{\"count\":2,\"width\":1000}}]}");
	}
	catch (const std::runtime_error &)
	{
		threw = true;
	}
	CHECK(threw, "a bad sectionControl is refused");
}

static sim::Profile two_sections(double on, double off)
{
	auto p = sim::parse_profile("{\"name\":\"t\",\"type\":\"sprayer\",\"sectionDelay\":{\"on\":" + std::to_string(on) + ",\"off\":" + std::to_string(off) + "},"
	                            "\"booms\":[{\"sections\":{\"count\":2,\"width\":1000}}],"
	                            "\"products\":[{\"name\":\"p\",\"unit\":\"l/ha\",\"rate\":100,\"min\":10,\"max\":500,\"bin\":{\"capacity\":1000}}]}");
	return p;
}

static void test_delays()
{
	// The delay counts from the first tick that sees the command (MODEL.md section 8), here 20 ms after it is set.
	sim::Model m(two_sections(0.3, 0.2));
	m.tick(0);
	m.set_section_setpoint(0, 0, true);
	for (std::uint32_t t = 20; t < 320; t += 20)
	{
		m.tick(t);
		CHECK(!m.section(0, 0).actual, "section on before its delay at %u ms", t);
	}
	m.tick(320);
	CHECK(m.section(0, 0).actual, "section on at exactly the delay");
	CHECK(m.boom(0).generation == 1, "generation bumped once, is %llu", static_cast<unsigned long long>(m.boom(0).generation));
	CHECK(m.actual_string(0) == "10" && m.setpoint_string(0) == "10", "strings %s %s", m.actual_string(0).c_str(), m.setpoint_string(0).c_str());

	m.set_section_setpoint(0, 0, false);
	for (std::uint32_t t = 340; t < 540; t += 20)
	{
		m.tick(t);
		CHECK(m.section(0, 0).actual, "section off before its delay at %u ms", t);
	}
	m.tick(540);
	CHECK(!m.section(0, 0).actual, "section off at exactly the off delay");

	// A reversal within the delay cancels the change
	m.set_section_setpoint(0, 1, true);
	m.tick(560);
	m.tick(640);
	m.set_section_setpoint(0, 1, false);
	m.tick(660);
	for (std::uint32_t t = 680; t <= 1200; t += 20)
	{
		m.tick(t);
	}
	CHECK(!m.section(0, 1).actual, "a reversed command never switched the section");
	// And the next command starts its delay afresh
	m.set_section_setpoint(0, 1, true);
	m.tick(1220);
	m.tick(1500);
	CHECK(!m.section(0, 1).actual, "fresh delay not yet over at 280 ms");
	m.tick(1520);
	CHECK(m.section(0, 1).actual, "fresh delay over at 300 ms");
}

static void test_modes()
{
	sim::Model m(two_sections(0.0, 0.0));
	m.tick(0);
	m.set_section_setpoint(0, 0, true);
	m.tick(20);
	CHECK(m.section(0, 0).actual && !m.section(0, 1).actual, "auto: follows the setpoint");
	m.set_switch(0, 0, false);
	m.tick(40);
	CHECK(!m.section(0, 0).actual, "auto: the operator's switch off wins");
	m.set_switch(0, 0, true);
	m.set_auto(0, false);
	m.tick(60);
	CHECK(m.section(0, 0).actual && m.section(0, 1).actual, "manual: switches rule, setpoints ignored: %s", m.actual_string(0).c_str());
	m.set_master(0, false);
	m.tick(80);
	CHECK(m.actual_string(0) == "00", "master off: all off");
	m.set_master(0, true);
	m.set_auto(0, true);
	m.set_section_error(0, 1, true);
	m.set_section_setpoint(0, 1, true);
	m.tick(100);
	CHECK(m.actual_string(0) == "1E", "error section reports E: %s", m.actual_string(0).c_str());
	CHECK(m.condensed_actual(0, 0) == (0x01u | (0x02u << 2) | 0xFFFFFFF0u), "condensed actual 0x%08X", m.condensed_actual(0, 0));
	CHECK(m.condensed_setpoint(0, 0) == (0x01u | (0x01u << 2) | 0xFFFFFFF0u), "condensed setpoint 0x%08X", m.condensed_setpoint(0, 0));
	CHECK(m.condensed_chunks(0) == 1, "chunks");
}

static void test_condensed_padding()
{
	auto p = sim::parse_profile("{\"name\":\"t\",\"type\":\"sprayer\",\"sectionDelay\":{\"on\":0,\"off\":0},\"booms\":[{\"sections\":{\"count\":20,\"width\":1000}}]}");
	sim::Model m(p);
	m.tick(0);
	for (std::size_t i = 0; i < 20; i += 2)
	{
		m.set_section_setpoint(0, i, true);
	}
	m.tick(20);
	CHECK(m.condensed_chunks(0) == 2, "20 sections are two chunks");
	CHECK(m.condensed_actual(0, 0) == 0x11111111u, "chunk 0 0x%08X", m.condensed_actual(0, 0));
	CHECK(m.condensed_actual(0, 1) == (0x11u | 0xFFFFFF00u), "chunk 1 0x%08X", m.condensed_actual(0, 1));
	CHECK(m.width_on_mm(0) == 10000 && m.working_width_mm(0) == 20000, "widths %d %d", m.width_on_mm(0), m.working_width_mm(0));
}

static void test_totals_and_bin(const std::string &dir)
{
	// One hectare with planter-12 at 10 km/h: 1 093.6 m in 393.7 s (SEEDER.md section 8)
	auto p = sim::load_profile(dir + "/planter-12.json");
	p.delayOn = 0;
	p.delayOff = 0;
	sim::Model m(p);
	m.tick(0);
	for (std::size_t i = 0; i < 12; i++)
	{
		m.set_section_setpoint(0, i, true);
	}
	const double speed = 10000.0 / 3.6; // mm/s
	std::uint32_t t = 0;
	const std::uint32_t end = 393700;
	while (t < end)
	{
		t += 20;
		m.set_speed(speed, t);
		m.tick(t);
	}
	CHECK(near(m.boom(0).areaM2, 10000.0, 100.0), "area after one hectare %.1f m2", m.boom(0).areaM2);
	CHECK(near(m.boom(0).effectiveTimeS, 393.7, 0.1), "effective time %.1f s", m.boom(0).effectiveTimeS);
	CHECK(near(m.boom(0).effectiveDistanceMm, 1093611.0, 11000.0), "effective distance %.0f mm", m.boom(0).effectiveDistanceMm);
	CHECK(near(m.product(0).tripTotal, 80000.0, 1600.0), "seeds applied %.0f", m.product(0).tripTotal);
	CHECK(near(m.product(0).level, 1500000.0 - 80000.0, 1600.0), "hopper level %.0f", m.product(0).level);
	CHECK(near(m.product(0).actualRate, 80000.0, 80000.0 * 0.05), "actual rate while applying %.0f", m.product(0).actualRate);

	// Speed goes stale after two seconds: nothing accrues
	const double areaBefore = m.boom(0).areaM2;
	for (int i = 0; i < 150; i++)
	{
		t += 20;
		m.tick(t);
	}
	CHECK(!m.speed_known(), "speed unknown after 3 s of silence");
	CHECK(near(m.boom(0).areaM2, areaBefore, 10000.0 / 1093.6 * 2.1 * 9.144 + 1.0), "area stops accruing without speed");
	CHECK(m.product(0).actualRate < 80000.0 * 0.01, "no rate without speed (%.0f)", m.product(0).actualRate);

	// Rates: the TC's setpoint is clipped to the limits; prescription off hands over to the operator
	CHECK(m.set_tc_rate(0, 500000), "a rate above the maximum is clipped");
	CHECK(near(m.target_rate(0), 200000, 0.1), "clipped to the maximum: %.0f", m.target_rate(0));
	m.set_prescription(0, false);
	m.set_operator_rate(0, 60000);
	CHECK(near(m.target_rate(0), 60000, 0.1), "operator rate rules with prescription off: %.0f", m.target_rate(0));
	m.reset_trip_total(0);
	CHECK(m.product(0).tripTotal == 0.0 && m.product(0).lifetimeTotal > 0, "trip reset leaves the lifetime total");
}

static void test_sprayer_model(const std::string &dir)
{
	auto p = sim::load_profile(dir + "/sprayer-24.json");
	p.delayOn = 0;
	p.delayOff = 0;
	p.products[0].noise = 0; // the numbers below are exact without it
	sim::Model m(p);
	m.tick(0);
	for (std::size_t i = 0; i < 12; i++)
	{
		m.set_section_setpoint(0, i, true);
	}
	std::uint32_t t = 0;
	auto run = [&](double seconds) {
		for (double d = 0; d < seconds; d += 0.02)
		{
			t += 20;
			m.set_speed(12000.0 / 3.6, t);
			m.tick(t);
		}
	};
	// The rate rises with the 1 s time constant: 63 % after 1 s, within 2 % after 5 s
	run(1.0);
	CHECK(near(m.product(0).actualRate, 150.0 * 0.632, 3.0), "rate after one time constant %.1f l/ha", m.product(0).actualRate);
	run(4.0);
	CHECK(near(m.product(0).actualRate, 150.0, 3.0), "rate settled %.1f l/ha", m.product(0).actualRate);
	// 150 l/ha × 24 m × 12 km/h / 600 = 72 l/min; 1.5 l/min a nozzle; 3 × (1.5/1.6)² = 2.64 bar
	CHECK(near(m.product(0).flowLPerMin, 72.0, 1.5), "flow %.2f l/min", m.product(0).flowLPerMin);
	CHECK(near(m.product(0).pressureBar, 2.637, 0.1), "pressure %.2f bar", m.product(0).pressureBar);
	// A step to 200: 4.7 bar once settled
	m.set_tc_rate(0, 200);
	run(5.0);
	CHECK(near(m.product(0).actualRate, 200.0, 4.0), "rate after a step %.1f", m.product(0).actualRate);
	CHECK(near(m.product(0).pressureBar, 4.69, 0.2), "pressure after the step %.2f bar", m.product(0).pressureBar);
	// The pump's limit: 60 l/min at 24 m and 12 km/h is 125 l/ha
	p.sprayer.pumpMaxFlow = 60;
	sim::Model m2(p);
	m2.tick(0);
	for (std::size_t i = 0; i < 12; i++)
	{
		m2.set_section_setpoint(0, i, true);
	}
	t = 0;
	for (double d = 0; d < 8.0; d += 0.02)
	{
		t += 20;
		m2.set_speed(12000.0 / 3.6, t);
		m2.tick(t);
	}
	CHECK(m2.product(0).limited && near(m2.product(0).actualRate, 125.0, 3.0), "pump limit: %.1f l/ha against a 150 target, limited=%d", m2.product(0).actualRate, m2.product(0).limited);

	// Noise: with it on, the actual wanders around the target by about the profile's fraction
	auto pn = sim::load_profile(dir + "/sprayer-24.json");
	pn.delayOn = 0;
	pn.delayOff = 0;
	sim::Model m3(pn);
	m3.seed_noise(7);
	m3.tick(0);
	for (std::size_t i = 0; i < 12; i++)
	{
		m3.set_section_setpoint(0, i, true);
	}
	t = 0;
	double sum = 0, sumSq = 0;
	int n = 0;
	for (double d = 0; d < 30.0; d += 0.02)
	{
		t += 20;
		m3.set_speed(12000.0 / 3.6, t);
		m3.tick(t);
		if (d > 10.0)
		{
			sum += m3.product(0).actualRate;
			sumSq += m3.product(0).actualRate * m3.product(0).actualRate;
			n++;
		}
	}
	const double mean = sum / n, sd = std::sqrt(std::max(0.0, sumSq / n - mean * mean));
	CHECK(near(mean, 150.0, 1.0) && sd > 1.0 && sd < 6.0, "noise: mean %.1f, sd %.2f (2 %% of 150 is 3)", mean, sd);
}

static void test_seeder_limits(const std::string &dir)
{
	// The planter's meter cannot keep up above 16 km/h: at 20 km/h the rate is 16/20 of the target
	auto p = sim::load_profile(dir + "/planter-12.json");
	p.delayOn = 0;
	p.delayOff = 0;
	p.products[0].noise = 0;
	sim::Model m(p);
	m.tick(0);
	for (std::size_t i = 0; i < 12; i++)
	{
		m.set_section_setpoint(0, i, true);
	}
	std::uint32_t t = 0;
	for (double d = 0; d < 6.0; d += 0.02)
	{
		t += 20;
		m.set_speed(20000.0 / 3.6, t);
		m.tick(t);
	}
	CHECK(m.product(0).limited && near(m.product(0).actualRate, 64000.0, 1500.0), "meter limit at 20 km/h: %.0f seeds/ha", m.product(0).actualRate);
	// Singulation falls above the 10 km/h knee: 98.5 % minus 1 % a km/h is 88.5 % at 20
	double s = 0;
	for (std::size_t i = 0; i < 12; i++)
	{
		s += m.row_singulation(0, i);
	}
	CHECK(near(s / 12.0, 0.885, 0.02), "singulation at 20 km/h %.3f", s / 12.0);

	// The drill's delivery delay holds the rate for the off delay after a section is commanded off
	auto d = sim::load_profile(dir + "/drill-6m.json");
	d.products[0].noise = 0;
	d.products[1].noise = 0;
	sim::Model md(d);
	md.tick(0);
	for (std::size_t i = 0; i < 4; i++)
	{
		md.set_section_setpoint(0, i, true);
	}
	t = 0;
	for (double x = 0; x < 10.0; x += 0.02)
	{
		t += 20;
		md.set_speed(10000.0 / 3.6, t);
		md.tick(t);
	}
	CHECK(near(md.product(0).actualRate, 180.0, 2.0) && near(md.product(1).actualRate, 100.0, 2.0), "drill: both products at their rates (%.1f, %.1f)", md.product(0).actualRate, md.product(1).actualRate);
	for (std::size_t i = 0; i < 4; i++)
	{
		md.set_section_setpoint(0, i, false);
	}
	const double levelBefore = md.product(0).level;
	for (double x = 0; x < 0.7; x += 0.02)
	{
		t += 20;
		md.set_speed(10000.0 / 3.6, t);
		md.tick(t);
	}
	CHECK(md.any_actual_on(0) && md.product(0).level < levelBefore, "drill: 0.7 s after all off the seed is still flowing (off delay 0.8 s)");
	for (double x = 0; x < 6.0; x += 0.02)
	{
		t += 20;
		md.set_speed(10000.0 / 3.6, t);
		md.tick(t);
	}
	CHECK(!md.any_actual_on(0) && md.product(0).actualRate < 1.0, "drill: then off, rate %.2f", md.product(0).actualRate);
}

static void test_ddop(const std::string &dir)
{
	isobus::NAME name(0);
	name.set_identity_number(1);
	name.set_manufacturer_code(1407);
	const char *files[] = { "planter-12", "planter-24", "planter-48", "drill-6m", "airseeder-12m", "sprayer-24", "sprayer-36-nozzle", "sprayer-120ft", "sprayer-mounted-12" };
	for (const char *f : files)
	{
		auto p = sim::load_profile(dir + "/" + f + ".json");
		for (auto mode : { sim::SectionControl::Setpoint, sim::SectionControl::ActualSettable, sim::SectionControl::PerElement })
		{
			p.sectionControl = mode;
			sim::Ddop d;
			try
			{
				d = sim::build_ddop(p, name, "0.1.0");
			}
			catch (const std::exception &e)
			{
				CHECK(false, "%s (%s): %s", f, sim::to_string(mode), e.what());
				continue;
			}
			CHECK(d.structureLabel.size() == 7, "%s structure label '%s'", f, d.structureLabel.c_str());
			CHECK(d.map.booms.size() == p.booms.size(), "%s boom elements", f);
			CHECK(d.map.bins.size() == p.products.size(), "%s bin elements", f);
			std::size_t sections = 0;
			for (const auto &s : d.map.sections)
			{
				sections += s.size();
			}
			CHECK(sections == p.total_sections(), "%s section elements %zu", f, sections);
			std::vector<std::uint8_t> binary;
			CHECK(d.pool->generate_binary_object_pool(binary), "%s (%s): the stack serialises it", f, sim::to_string(mode));
			CHECK(binary.size() > 100, "%s binary size %zu", f, binary.size());
			std::size_t boom, index;
			CHECK(d.map.section_for(d.map.sections[0][0], boom, index) && boom == 0 && index == 0, "%s element lookup", f);
			if (mode == sim::SectionControl::Setpoint && std::string(f) == "planter-48")
			{
				std::printf("info  planter-48 DDOP: %zu objects, %zu bytes\n", d.objectCount, binary.size());
			}
		}
	}
	// Round trip through the stack's serializer: every element's children exist and belong to one element only
	for (const char *f : files)
	{
		auto p = sim::load_profile(dir + "/" + f + ".json");
		auto d = sim::build_ddop(p, name, "0.1.0");
		d.pool->set_task_controller_compatibility_level(3);
		std::vector<std::uint8_t> binary;
		d.pool->generate_binary_object_pool(binary);
		if (argc_copy > 3)
		{
			// Check a DDOP file a task controller received instead (argv[3]), for the planter-48 profile
			if (std::string(f) != "planter-48")
			{
				continue;
			}
			FILE *in = std::fopen(argv_copy[3], "rb");
			binary.clear();
			int c;
			while (in && (c = std::fgetc(in)) != EOF)
			{
				binary.push_back(static_cast<std::uint8_t>(c));
			}
			if (in)
			{
				std::fclose(in);
			}
			std::printf("info  checking %s, %zu bytes\n", argv_copy[3], binary.size());
		}
		isobus::DeviceDescriptorObjectPool parsed;
		parsed.set_task_controller_compatibility_level(3);
		// A received file carries the real implement's NAME; NAME 0 skips the check, as a task controller would pass the sender's
		CHECK(parsed.deserialize_binary_object_pool(binary.data(), static_cast<std::uint32_t>(binary.size()), argc_copy > 3 ? isobus::NAME(0) : name), "%s: the stack parses its own binary", f);
		CHECK(parsed.size() == d.objectCount, "%s: %u objects parsed of %zu", f, parsed.size(), d.objectCount);
		std::map<std::uint16_t, std::uint16_t> owner; // child id -> element id
		std::size_t elements = 0, problems = 0;
		for (std::uint16_t i = 0; i < parsed.size(); i++)
		{
			auto object = parsed.get_object_by_index(i);
			if (!object || object->get_object_type() != isobus::task_controller_object::ObjectTypes::DeviceElement)
			{
				continue;
			}
			elements++;
			auto element = std::static_pointer_cast<isobus::task_controller_object::DeviceElementObject>(object);
			for (std::uint16_t child : element->get_child_object_ids())
			{
				if (!parsed.get_object_by_id(child))
				{
					problems++;
					std::printf("info  %s: element %u (object %u) references missing object %u\n", f, element->get_element_number(), element->get_object_id(), child);
				}
				auto found = owner.find(child);
				if (found != owner.end())
				{
					problems++;
					std::printf("info  %s: object %u is a child of elements (objects) %u and %u\n", f, child, found->second, element->get_object_id());
				}
				owner[child] = element->get_object_id();
			}
		}
		CHECK(problems == 0, "%s: %zu child reference problems over %zu elements", f, problems, elements);
		if (std::string(f) == "planter-48" && argc_copy > 2)
		{
			FILE *out = std::fopen(argv_copy[2], "wb");
			if (out)
			{
				std::fwrite(binary.data(), 1, binary.size(), out);
				std::fclose(out);
			}
		}
	}

	// A changed geometry changes the structure label
	auto a = sim::load_profile(dir + "/sprayer-24.json");
	auto b = a;
	b.booms[0].sections[3].width += 1;
	CHECK(sim::build_ddop(a, name, "0.1.0").structureLabel != sim::build_ddop(b, name, "0.1.0").structureLabel, "structure label follows the geometry");
	CHECK(sim::build_ddop(a, name, "0.1.0").structureLabel == sim::build_ddop(a, name, "0.1.0").structureLabel, "structure label is stable");
}

int main(int argc, char **argv)
{
	argc_copy = argc;
	argv_copy = argv;
	const std::string dir = argc > 1 ? argv[1] : "profiles";
	try
	{
		test_profiles(dir);
		test_delays();
		test_modes();
		test_condensed_padding();
		test_totals_and_bin(dir);
		test_sprayer_model(dir);
		test_seeder_limits(dir);
		test_ddop(dir);
	}
	catch (const std::exception &e)
	{
		std::printf("FAIL  exception: %s\n", e.what());
		failures++;
	}
	std::printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
