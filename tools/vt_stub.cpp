// vt_stub: a headless virtual terminal server for the scenarios (docs/PLAN.md phase 3). It
// takes a working set's pool, reports what the client does as JSON lines on stdout, and
// injects what an operator would do from lines on stdin:
//
//   key <object id>      a soft key pressed and released
//   button <object id>   a button pressed and released
//   ack                  the alarm acknowledge (soft key activation, key number 0)
//   value <id> <n>       a value entered (VT change numeric value)
//   assign <function id> <input id>
//                        an AUX-N assignment, as an operator makes on a terminal that does AUX-N:
//                        the function's working set is told the input's device NAME and object
//                        (Auxiliary Assignment Type 2 command); the stack's server has no AUX-N
//   unassign <function id>
//   quit
//
//   vt_stub [--interface vcan0] [--driver socketcan|virtual|aowlink]
#include "can_driver.hpp"
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server_managed_working_set.hpp"

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
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

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

	/// A terminal of VT version 3, 480 x 480, six soft keys of 60 x 60, pools kept in memory.
	class StubServer : public isobus::VirtualTerminalServer
	{
	public:
		explicit StubServer(std::shared_ptr<isobus::InternalControlFunction> cf) :
		  isobus::VirtualTerminalServer(cf) {}

		bool get_is_enough_memory(std::uint32_t) const override { return true; }
		VTVersion get_version() const override { return VTVersion::Version3; }
		std::uint8_t get_number_of_navigation_soft_keys() const override { return 6; }
		std::uint8_t get_soft_key_descriptor_x_pixel_width() const override { return 60; }
		std::uint8_t get_soft_key_descriptor_y_pixel_height() const override { return 60; }
		std::uint8_t get_number_of_possible_virtual_soft_keys_in_soft_key_mask() const override { return 64; }
		std::uint8_t get_number_of_physical_soft_keys() const override { return 6; }
		std::uint16_t get_data_mask_area_size_x_pixels() const override { return 480; }
		std::uint16_t get_data_mask_area_size_y_pixels() const override { return 480; }
		void suspend_working_set(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws) override
		{
			emit("suspended", { { "name", name_of(ws) } });
		}
		SupportedWideCharsErrorCode get_supported_wide_chars(std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t &numberOfRanges, std::vector<std::uint8_t> &) override
		{
			numberOfRanges = 0;
			return SupportedWideCharsErrorCode{};
		}
		std::vector<std::array<std::uint8_t, 7>> get_versions(isobus::NAME clientNAME) override
		{
			std::vector<std::array<std::uint8_t, 7>> out;
			for (const auto &v : stored[clientNAME.get_full_name()])
			{
				std::array<std::uint8_t, 7> label{};
				for (std::size_t i = 0; i < 7 && i < v.first.size(); i++)
				{
					label[i] = static_cast<std::uint8_t>(v.first[i]);
				}
				out.push_back(label);
			}
			return out;
		}
		std::vector<std::array<std::uint8_t, 32>> get_extended_versions(isobus::NAME) override { return {}; }
		std::vector<std::uint8_t> get_supported_objects() const override
		{
			std::vector<std::uint8_t> out;
			for (std::uint8_t t = 0; t <= 45; t++)
			{
				out.push_back(t);
			}
			return out;
		}
		std::vector<std::uint8_t> load_version(const std::vector<std::uint8_t> &label, isobus::NAME clientNAME) override
		{
			const std::string key(label.begin(), label.end());
			auto &mine = stored[clientNAME.get_full_name()];
			auto found = mine.find(key);
			emit("loadVersion", { { "label", key }, { "found", found != mine.end() } });
			return found != mine.end() ? found->second : std::vector<std::uint8_t>();
		}
		bool save_version(const std::vector<std::uint8_t> &pool, const std::vector<std::uint8_t> &label, isobus::NAME clientNAME) override
		{
			const std::string key(label.begin(), label.end());
			stored[clientNAME.get_full_name()][key] = pool;
			emit("saveVersion", { { "label", key }, { "bytes", pool.size() } });
			return true;
		}
		bool delete_version(const std::vector<std::uint8_t> &label, isobus::NAME clientNAME) override
		{
			const std::string key(label.begin(), label.end());
			emit("deleteVersion", { { "label", key } });
			return stored[clientNAME.get_full_name()].erase(key) > 0;
		}
		bool delete_all_versions(std::uint8_t, isobus::NAME clientNAME) override
		{
			stored[clientNAME.get_full_name()].clear();
			return true;
		}
		bool delete_object_pool(isobus::NAME clientNAME) override
		{
			emit("deletePool", { { "name", std::to_string(clientNAME.get_full_name()) } });
			return true;
		}

		static std::string name_of(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &ws)
		{
			auto cf = ws ? ws->get_control_function() : nullptr;
			return cf ? std::to_string(cf->get_NAME().get_full_name()) : "?";
		}

		/// The working sets the server manages, for the loop below
		std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> working_sets()
		{
			return managedWorkingSetList;
		}

		void tick()
		{
			update();
		}

	private:
		std::map<std::uint64_t, std::map<std::string, std::vector<std::uint8_t>>> stored;
	};

	void signal_handler(int)
	{
		running = false;
	}

	std::uint16_t active_mask_of(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &ws)
	{
		auto object = ws->get_working_set_object();
		if (object && object->get_object_type() == isobus::VirtualTerminalObjectType::WorkingSet)
		{
			return std::static_pointer_cast<isobus::WorkingSet>(object)->get_active_mask();
		}
		return 0xFFFF;
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
			std::fprintf(stderr, "usage: vt_stub [--interface vcan0] [--driver socketcan|virtual|aowlink]\n");
			return 2;
		}
	}
	StderrLogger logger;
	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Warning);
	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);

	auto driver = sim::create_can_driver(interface, driverName, "vt_stub");
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
	name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal));
	name.set_identity_number(900);
	name.set_manufacturer_code(1407);
	auto cf = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0, 0x26);
	StubServer server(cf);
	server.get_on_change_active_mask_event_dispatcher().add_listener([](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws, std::uint16_t a, std::uint16_t b) {
		emit("mask", { { "name", StubServer::name_of(ws) }, { "mask", b }, { "previous", a } });
	});
	server.initialize();
	emit("ready", { { "interface", interface }, { "version", 3 }, { "width", 480 }, { "height", 480 } });

	// Commands from stdin on their own thread
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

	std::map<std::uint64_t, std::string> lastState;
	std::map<std::uint64_t, std::uint16_t> lastMask;
	std::map<std::uint64_t, std::map<std::uint16_t, std::uint32_t>> lastValues;
	std::uint32_t lastScan = 0;
	while (running)
	{
		server.tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));

		// Working sets: parsing state, active mask, numeric values the client changes
		for (const auto &ws : server.working_sets())
		{
			auto cfOf = ws->get_control_function();
			if (!cfOf)
			{
				continue;
			}
			const std::uint64_t id = cfOf->get_NAME().get_full_name();
			std::string state = "none";
			using S = isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState;
			switch (ws->get_object_pool_processing_state())
			{
				case S::Running:
					state = "parsing";
					break;
				case S::Success:
				case S::Joined:
					state = "loaded";
					break;
				case S::Fail:
					state = "failed";
					break;
				default:
					break;
			}
			if (lastState[id] != state)
			{
				lastState[id] = state;
				emit("pool", { { "name", std::to_string(id) }, { "state", state }, { "address", cfOf->get_address() }, { "fromMemory", ws->get_was_object_pool_loaded_from_non_volatile_memory() } });
			}
			if (state == "loaded")
			{
				const std::uint16_t mask = active_mask_of(ws);
				if (lastMask.find(id) == lastMask.end() || lastMask[id] != mask)
				{
					lastMask[id] = mask;
					emit("mask", { { "name", std::to_string(id) }, { "mask", mask } });
				}
				const auto now = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
				if (now - lastScan >= 100)
				{
					lastScan = now;
					for (const auto &entry : ws->get_object_tree())
					{
						if (!entry.second)
						{
							continue;
						}
						// Values the client changes: number variables, and output numbers, object pointers and bar graphs that hold their own
						std::uint32_t value;
						switch (entry.second->get_object_type())
						{
							case isobus::VirtualTerminalObjectType::NumberVariable:
								value = std::static_pointer_cast<isobus::NumberVariable>(entry.second)->get_value();
								break;
							case isobus::VirtualTerminalObjectType::OutputNumber:
								value = std::static_pointer_cast<isobus::OutputNumber>(entry.second)->get_value();
								break;
							case isobus::VirtualTerminalObjectType::ObjectPointer:
								value = std::static_pointer_cast<isobus::ObjectPointer>(entry.second)->get_value();
								break;
							case isobus::VirtualTerminalObjectType::OutputLinearBarGraph:
								value = std::static_pointer_cast<isobus::OutputLinearBarGraph>(entry.second)->get_value();
								break;
							default:
								continue;
						}
						{
							auto &known = lastValues[id];
							auto found = known.find(entry.first);
							if (found == known.end() || found->second != value)
							{
								known[entry.first] = value;
								emit("change", { { "name", std::to_string(id) }, { "id", entry.first }, { "value", value } });
							}
						}
					}
				}
			}
		}

		// Operator actions
		std::vector<std::string> commands;
		{
			std::lock_guard<std::mutex> lock(commandMutex);
			commands.swap(pending);
		}
		for (const auto &line : commands)
		{
			std::string cmd, a1, a2;
			{
				std::size_t p = 0;
				auto word = [&](std::string &out) {
					while (p < line.size() && line[p] == ' ') p++;
					const std::size_t s = p;
					while (p < line.size() && line[p] != ' ') p++;
					out = line.substr(s, p - s);
				};
				word(cmd);
				word(a1);
				word(a2);
			}
			if (cmd == "quit")
			{
				running = false;
				break;
			}
			std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws = server.get_active_working_set();
			if (!ws)
			{
				for (const auto &candidate : server.working_sets())
				{
					ws = candidate;
				}
			}
			if (!ws || !ws->get_control_function())
			{
				emit("command", { { "command", line }, { "ok", false }, { "error", "no working set" } });
				continue;
			}
			auto dest = ws->get_control_function();
			const std::uint16_t mask = active_mask_of(ws);
			using Code = isobus::VirtualTerminalBase::KeyActivationCode;
			bool ok = true;
			if (cmd == "key" || cmd == "button")
			{
				const std::uint16_t id = static_cast<std::uint16_t>(std::atoi(a1.c_str()));
				if (cmd == "key")
				{
					ok = server.send_soft_key_activation_message(Code::ButtonPressedOrLatched, id, mask, 1, dest);
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					ok = server.send_soft_key_activation_message(Code::ButtonUnlatchedOrReleased, id, mask, 1, dest) && ok;
				}
				else
				{
					ok = server.send_button_activation_message(Code::ButtonPressedOrLatched, id, mask, 1, dest);
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					ok = server.send_button_activation_message(Code::ButtonUnlatchedOrReleased, id, mask, 1, dest) && ok;
				}
			}
			else if (cmd == "ack")
			{
				ok = server.send_soft_key_activation_message(Code::ButtonUnlatchedOrReleased, mask, mask, 0, dest);
			}
			else if (cmd == "value")
			{
				ok = server.send_change_numeric_value_message(static_cast<std::uint16_t>(std::atoi(a1.c_str())), static_cast<std::uint32_t>(std::atol(a2.c_str())), dest);
			}
			else if (cmd == "assign" || cmd == "unassign")
			{
				// The working sets whose pools have the function and the input
				using Type = isobus::VirtualTerminalObjectType;
				const std::uint16_t functionId = static_cast<std::uint16_t>(std::atoi(a1.c_str()));
				const std::uint16_t inputId = cmd == "assign" ? static_cast<std::uint16_t>(std::atoi(a2.c_str())) : 0xFFFF;
				std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> functionSet, inputSet;
				std::uint8_t functionType = 0x1F; // unassign
				for (const auto &candidate : server.working_sets())
				{
					auto function = candidate->get_object_by_id(functionId);
					if (function && function->get_object_type() == Type::AuxiliaryFunctionType2)
					{
						functionSet = candidate;
						if (cmd == "assign")
						{
							functionType = static_cast<std::uint8_t>(std::static_pointer_cast<isobus::AuxiliaryFunctionType2>(function)->get_function_type());
						}
					}
					auto input = cmd == "assign" ? candidate->get_object_by_id(inputId) : nullptr;
					if (input && input->get_object_type() == Type::AuxiliaryInputType2)
					{
						inputSet = candidate;
					}
				}
				ok = functionSet && functionSet->get_control_function() && (cmd == "unassign" || (inputSet && inputSet->get_control_function()));
				if (ok)
				{
					const std::uint64_t inputName = cmd == "assign" ? inputSet->get_control_function()->get_NAME().get_full_name() : 0xFFFFFFFFFFFFFFFFULL;
					std::uint8_t data[14] = { 0x24 }; // Auxiliary Assignment Type 2 command
					for (int i = 0; i < 8; i++)
					{
						data[1 + i] = static_cast<std::uint8_t>(inputName >> (8 * i));
					}
					data[9] = functionType; // bit 7, store as preferred, left clear
					data[10] = static_cast<std::uint8_t>(inputId & 0xFF);
					data[11] = static_cast<std::uint8_t>(inputId >> 8);
					data[12] = static_cast<std::uint8_t>(functionId & 0xFF);
					data[13] = static_cast<std::uint8_t>(functionId >> 8);
					ok = isobus::CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU), data, sizeof(data), cf, functionSet->get_control_function());
				}
			}
			else
			{
				ok = false;
			}
			emit("command", { { "command", line }, { "ok", ok } });
		}
	}
	isobus::CANHardwareInterface::stop();
	std::fclose(stdin);
	if (reader.joinable())
	{
		reader.detach();
	}
	emit("exit");
	return 0;
}
