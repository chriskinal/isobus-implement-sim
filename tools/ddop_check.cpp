// ddop_check: parses a binary DDOP file with the stack and prints its element tree and any
// broken child references. For looking at what a task controller received (phase 1 record)
// and, later, at captures (PLAN.md phase 6).
//
//     ddop_check <file.ddop> [tc version, default 3]
#include "isobus/isobus/isobus_device_descriptor_object_pool.hpp"
#include "isobus/isobus/isobus_task_controller_client_objects.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: ddop_check <file.ddop> [tc version]\n");
		return 2;
	}
	std::vector<std::uint8_t> binary;
	FILE *in = std::fopen(argv[1], "rb");
	if (!in)
	{
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	int c;
	while ((c = std::fgetc(in)) != EOF)
	{
		binary.push_back(static_cast<std::uint8_t>(c));
	}
	std::fclose(in);

	isobus::DeviceDescriptorObjectPool pool;
	pool.set_task_controller_compatibility_level(static_cast<std::uint8_t>(argc > 2 ? std::atoi(argv[2]) : 3));
	if (!pool.deserialize_binary_object_pool(binary.data(), static_cast<std::uint32_t>(binary.size()), isobus::NAME(0)))
	{
		std::printf("parse failed after %u objects of %zu bytes\n", pool.size(), binary.size());
		return 1;
	}
	std::printf("%zu bytes, %u objects\n", binary.size(), pool.size());

	using namespace isobus::task_controller_object;
	std::map<std::uint16_t, std::uint16_t> owner;
	int problems = 0;
	for (std::uint16_t i = 0; i < pool.size(); i++)
	{
		auto object = pool.get_object_by_index(i);
		if (!object || object->get_object_type() != ObjectTypes::DeviceElement)
		{
			continue;
		}
		auto element = std::static_pointer_cast<DeviceElementObject>(object);
		std::printf("element %u (object %u, type %d, parent %u) \"%s\": %u children:", element->get_element_number(), element->get_object_id(), static_cast<int>(element->get_type()), element->get_parent_object(), element->get_designator().c_str(), element->get_number_child_objects());
		for (std::uint16_t child : element->get_child_object_ids())
		{
			auto childObject = pool.get_object_by_id(child);
			if (!childObject)
			{
				std::printf(" %u(MISSING)", child);
				problems++;
				continue;
			}
			std::string tag;
			if (childObject->get_object_type() == ObjectTypes::DeviceProcessData)
			{
				tag = "pd" + std::to_string(std::static_pointer_cast<DeviceProcessDataObject>(childObject)->get_ddi());
			}
			else if (childObject->get_object_type() == ObjectTypes::DeviceProperty)
			{
				tag = "prop" + std::to_string(std::static_pointer_cast<DevicePropertyObject>(childObject)->get_ddi());
			}
			else
			{
				tag = "type" + std::to_string(static_cast<int>(childObject->get_object_type()));
			}
			std::printf(" %u:%s", child, tag.c_str());
			auto found = owner.find(child);
			if (found != owner.end())
			{
				std::printf("(ALSO in object %u)", found->second);
				problems++;
			}
			owner[child] = element->get_object_id();
		}
		std::printf("\n");
	}
	std::printf("%d problems\n", problems);
	return problems ? 1 : 0;
}
