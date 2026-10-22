// tc_stub: a headless task controller for the scenarios (docs/PLAN.md phase 5), on the stack's
// TaskControllerServer base after examples/task_controller_server (MIT). It takes any DDOP,
// reports what the implement sends as JSON lines on stdout, and sends what a task controller
// would from lines on stdin:
//
//   set <ddi> <element> <value>        a set-value command (a rate, a section word, a mode)
//   request <ddi> <element>            ask for a value once
//   interval <ddi> <element> <ms>      ask for it every ms
//   threshold <ddi> <element> <n>      ask for it when it changes by n
//   totals on|off                      the task-active bit in the status message
//   quit
//
//   tc_stub [--interface vcan0] [--driver socketcan|virtual]
#include "create_can_driver.hpp"
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_device_descriptor_object_pool.hpp"
#include "isobus/isobus/isobus_preferred_addresses.hpp"
#include "isobus/isobus/isobus_task_controller_server.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::atomic_bool running{ true };
	std::mutex outMutex;
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

	void emit(const std::string &name, nlohmann::json fields = nlohmann::json::object())
	{
		nlohmann::json line;
		line["t"] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
		line["event"] = name;
		for (auto &item : fields.items())
		{
			line[item.key()] = item.value();
		}
		std::lock_guard<std::mutex> lock(outMutex);
		std::fputs((line.dump() + "\n").c_str(), stdout);
		std::fflush(stdout);
	}

	class StderrLogger : public isobus::CANStackLogger
	{
	public:
		void sink_CAN_stack_log(LoggingLevel, const std::string &text) override
		{
			std::fprintf(stderr, "[stack] %s\n", text.c_str());
		}
	};

	std::string name_of(const std::shared_ptr<isobus::ControlFunction> &cf)
	{
		return cf ? std::to_string(cf->get_NAME().get_full_name()) : "?";
	}

	class StubServer : public isobus::TaskControllerServer
	{
	public:
		StubServer(std::shared_ptr<isobus::InternalControlFunction> cf) :
		  isobus::TaskControllerServer(cf, 4, 255, 16, isobus::TaskControllerOptions().with_documentation().with_implement_section_control().with_tc_geo_with_position_based_control()) {}

		bool activate_object_pool(std::shared_ptr<isobus::ControlFunction> cf, ObjectPoolActivationError &error, ObjectPoolErrorCodes &, std::uint16_t &, std::uint16_t &) override
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto &bytes = pending[cf];
			// The client formats its DDOP for the version this server reports: the stack's default, 4
			isobus::DeviceDescriptorObjectPool pool;
			pool.set_task_controller_compatibility_level(4);
			if (!pool.deserialize_binary_object_pool(bytes.data(), static_cast<std::uint32_t>(bytes.size()), cf->get_NAME()))
			{
				emit("client", { { "name", name_of(cf) }, { "activated", false }, { "bytes", bytes.size() }, { "error", "the DDOP does not parse" } });
				error = ObjectPoolActivationError::ThereAreErrorsInTheDDOP;
				return false;
			}
			// The element tree and the process data, for whoever drives this
			using namespace isobus::task_controller_object;
			nlohmann::json elements = nlohmann::json::array(), pds = nlohmann::json::array();
			std::string device;
			int sections = 0;
			for (std::uint16_t i = 0; i < pool.size(); i++)
			{
				auto object = pool.get_object_by_index(i);
				if (!object)
				{
					continue;
				}
				if (object->get_object_type() == ObjectTypes::Device)
				{
					device = object->get_designator();
				}
				else if (object->get_object_type() == ObjectTypes::DeviceElement)
				{
					auto element = std::static_pointer_cast<DeviceElementObject>(object);
					if (element->get_type() == DeviceElementObject::Type::Section)
					{
						sections++;
					}
					elements.push_back({ { "number", element->get_element_number() }, { "type", static_cast<int>(element->get_type()) }, { "designator", element->get_designator() } });
					for (std::uint16_t child : element->get_child_object_ids())
					{
						auto c = pool.get_object_by_id(child);
						if (c && c->get_object_type() == ObjectTypes::DeviceProcessData)
						{
							auto pd = std::static_pointer_cast<DeviceProcessDataObject>(c);
							pds.push_back({ { "element", element->get_element_number() }, { "ddi", pd->get_ddi() }, { "settable", pd->has_property(DeviceProcessDataObject::PropertiesBit::Settable) }, { "designator", pd->get_designator() } });
						}
					}
				}
			}
			emit("client", { { "name", name_of(cf) }, { "activated", true }, { "address", cf->get_address() }, { "device", device }, { "bytes", bytes.size() }, { "sections", sections }, { "elements", elements }, { "processData", pds } });
			pending.erase(cf);
			return true;
		}
		bool change_designator(std::shared_ptr<isobus::ControlFunction>, std::uint16_t, const std::vector<std::uint8_t> &) override { return true; }
		bool deactivate_object_pool(std::shared_ptr<isobus::ControlFunction> cf) override
		{
			emit("client", { { "name", name_of(cf) }, { "activated", false }, { "deactivated", true } });
			return true;
		}
		bool delete_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction> cf, ObjectPoolDeletionErrors &) override
		{
			std::lock_guard<std::mutex> lock(mutex);
			pending.erase(cf);
			return true;
		}
		bool get_is_stored_device_descriptor_object_pool_by_structure_label(std::shared_ptr<isobus::ControlFunction>, const std::vector<std::uint8_t> &, const std::vector<std::uint8_t> &) override { return false; }
		bool get_is_stored_device_descriptor_object_pool_by_localization_label(std::shared_ptr<isobus::ControlFunction>, const std::array<std::uint8_t, 7> &) override { return false; }
		bool get_is_enough_memory_available(std::uint32_t) override { return true; }
		void identify_task_controller(std::uint8_t) override {}
		void on_client_timeout(std::shared_ptr<isobus::ControlFunction> cf) override
		{
			emit("client", { { "name", name_of(cf) }, { "activated", false }, { "timeout", true } });
		}
		void on_process_data_acknowledge(std::shared_ptr<isobus::ControlFunction> cf, std::uint16_t ddi, std::uint16_t element, std::uint8_t errors, ProcessDataCommands command) override
		{
			emit("pdack", { { "name", name_of(cf) }, { "ddi", ddi }, { "element", element }, { "errors", errors }, { "command", static_cast<int>(command) } });
		}
		void on_client_version_received(std::shared_ptr<isobus::ControlFunction> cf, std::uint8_t version) override
		{
			emit("version", { { "name", name_of(cf) }, { "version", version } });
		}
		bool on_value_command(std::shared_ptr<isobus::ControlFunction> cf, std::uint16_t ddi, std::uint16_t element, std::int32_t value, std::uint8_t &) override
		{
			emit("value", { { "name", name_of(cf) }, { "ddi", ddi }, { "element", element }, { "value", value } });
			return true;
		}
		bool store_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction> cf, const std::vector<std::uint8_t> &data, bool append) override
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto &bytes = pending[cf];
			if (!append)
			{
				bytes.clear();
			}
			bytes.insert(bytes.end(), data.begin(), data.end());
			emit("ddop", { { "name", name_of(cf) }, { "bytes", bytes.size() }, { "append", append } });
			return true;
		}

		/// The first active client, for commands
		std::shared_ptr<isobus::ControlFunction> first_client()
		{
			for (const auto &c : activeClients)
			{
				if (c && c->isDDOPActive)
				{
					return c->clientControlFunction;
				}
			}
			return nullptr;
		}

	private:
		std::mutex mutex;
		std::map<std::shared_ptr<isobus::ControlFunction>, std::vector<std::uint8_t>> pending;
	};

	void signal_handler(int)
	{
		running = false;
	}
}

int main(int argc, char **argv)
{
	std::string interface, driverName;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--interface" && i + 1 < argc)
		{
			interface = argv[++i];
		}
		else if (a == "--driver" && i + 1 < argc)
		{
			driverName = argv[++i];
		}
		else
		{
			std::fprintf(stderr, "usage: tc_stub [--interface vcan0] [--driver socketcan|virtual]\n");
			return 2;
		}
	}
	StderrLogger logger;
	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Warning);
	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);

	auto driver = CANDriverFactory::create(interface, driverName);
	if (!driver)
	{
		return 1;
	}
	isobus::CANHardwareInterface::set_number_of_can_channels(1);
	isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, driver);
	if (!isobus::CANHardwareInterface::start() || !driver->get_is_valid())
	{
		std::fprintf(stderr, "could not open the CAN interface '%s'\n", interface.c_str());
		return 1;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(250));

	isobus::NAME name(0);
	name.set_arbitrary_address_capable(true);
	name.set_industry_group(2);
	name.set_device_class(0);
	name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::TaskController));
	name.set_identity_number(901);
	name.set_manufacturer_code(1407);
	auto cf = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0, isobus::preferred_addresses::IndustryGroup2::TaskController_MappingComputer);
	StubServer server(cf);
	server.get_language_command_interface().set_language_code("en");
	server.get_language_command_interface().set_country_code("GB");
	server.initialize();
	emit("ready", { { "interface", interface }, { "address", isobus::preferred_addresses::IndustryGroup2::TaskController_MappingComputer } });

	std::mutex commandMutex;
	std::vector<std::string> pending;
	std::thread reader([&] {
		std::string line;
		while (running && std::getline(std::cin, line))
		{
			std::lock_guard<std::mutex> lock(commandMutex);
			pending.push_back(line);
		}
	});

	while (running)
	{
		server.update();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		std::vector<std::string> commands;
		{
			std::lock_guard<std::mutex> lock(commandMutex);
			commands.swap(pending);
		}
		for (const auto &line : commands)
		{
			std::string words[4];
			std::size_t p = 0;
			for (auto &w : words)
			{
				while (p < line.size() && line[p] == ' ') p++;
				const std::size_t s = p;
				while (p < line.size() && line[p] != ' ') p++;
				w = line.substr(s, p - s);
			}
			const std::string &cmd = words[0];
			if (cmd == "quit")
			{
				running = false;
				break;
			}
			bool ok = false;
			if (cmd == "totals")
			{
				server.set_task_totals_active(words[1] == "on");
				ok = true;
			}
			else
			{
				auto client = server.first_client();
				if (!client)
				{
					emit("command", { { "command", line }, { "ok", false }, { "error", "no active client" } });
					continue;
				}
				const auto ddi = static_cast<std::uint16_t>(std::atoi(words[1].c_str()));
				const auto element = static_cast<std::uint16_t>(std::atoi(words[2].c_str()));
				const auto n = static_cast<std::uint32_t>(std::atol(words[3].c_str()));
				if (cmd == "set")
				{
					ok = server.send_set_value_and_acknowledge(client, ddi, element, n);
				}
				else if (cmd == "request")
				{
					ok = server.send_request_value(client, ddi, element);
				}
				else if (cmd == "interval")
				{
					ok = server.send_time_interval_measurement_command(client, ddi, element, n);
				}
				else if (cmd == "threshold")
				{
					ok = server.send_change_threshold_measurement_command(client, ddi, element, n);
				}
			}
			emit("command", { { "command", line }, { "ok", ok } });
		}
	}
	server.terminate();
	isobus::CANHardwareInterface::stop();
	std::fclose(stdin);
	if (reader.joinable())
	{
		reader.detach();
	}
	emit("exit");
	return 0;
}
