// aux_input: a simulated auxiliary input device, a joystick with one button and one slider
// (docs/PLAN.md phase 7), after AgIsoStack++ examples/virtual_terminal/aux_inputs (MIT). It
// uploads pools/aux_inputs.iop to the terminal and sends the input states a terminal has
// assigned to functions. Commands on stdin:
//
//   button on|off      the button, with its transition count
//   slider <0..64255>  the slider position
//   quit
//
//   aux_input --pool pools/aux_inputs.iop [--interface vcan0] [--driver socketcan|virtual]
//             [--joystick /dev/i2c-1 [--joystick-address 0x5a]]
//
// With --joystick (Linux), the I2C joystick module of tools/i2c_joystick.hpp drives the inputs
// too: its X axis is the slider, its A button the button. Every change of what the module
// reports, the other axis and buttons included, prints a "joystick" event with the raw values.
#include "../pools/aux_inputs_ids.hpp"
#include "create_can_driver.hpp"
#include "i2c_joystick.hpp"
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_partnered_control_function.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_client.hpp"
#include "isobus/utility/iop_file_interface.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
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

	void signal_handler(int)
	{
		running = false;
	}
}

int main(int argc, char **argv)
{
	std::string interface, driverName, poolPath = "pools/aux_inputs.iop", joystickDevice;
	long joystickAddress = I2CJoystick::DEFAULT_ADDRESS;
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
		else if (a == "--pool" && i + 1 < argc)
		{
			poolPath = argv[++i];
		}
		else if (a == "--joystick" && i + 1 < argc)
		{
			joystickDevice = argv[++i];
		}
		else if (a == "--joystick-address" && i + 1 < argc)
		{
			joystickAddress = std::strtol(argv[++i], nullptr, 0);
		}
		else
		{
			std::fprintf(stderr, "usage: aux_input [--pool pools/aux_inputs.iop] [--interface vcan0] [--driver socketcan|virtual] [--joystick /dev/i2c-1 [--joystick-address 0x5a]]\n");
			return 2;
		}
	}
	StderrLogger logger;
	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Warning);
	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);

	auto pool = isobus::IOPFileInterface::read_iop_file(poolPath);
	if (pool.empty())
	{
		std::fprintf(stderr, "cannot read the pool %s\n", poolPath.c_str());
		return 1;
	}
	// The joystick is checked before anything goes on the bus, and its centre taken
	I2CJoystick joystick;
	JoystickAxis axisX, axisY;
	I2CJoystick::Reading lastReading;
	if (!joystickDevice.empty())
	{
		if (!joystick.open(joystickDevice, static_cast<std::uint8_t>(joystickAddress)))
		{
			std::fprintf(stderr, "joystick: %s\n", joystick.error().c_str());
			return 1;
		}
		int sumX = 0, sumY = 0;
		for (int i = 0; i < 8; i++)
		{
			joystick.read(lastReading);
			sumX += lastReading.x;
			sumY += lastReading.y;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		axisX.set_centre(sumX / 8);
		axisY.set_centre(sumY / 8);
	}

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

	// An auxiliary input device: industry group 2, a generic function, its own identity
	isobus::NAME name(0);
	name.set_arbitrary_address_capable(true);
	name.set_industry_group(2);
	name.set_device_class(0);
	name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::AuxiliaryValveControl));
	name.set_identity_number(902);
	name.set_manufacturer_code(1407);
	const std::vector<isobus::NAMEFilter> vtFilters = { isobus::NAMEFilter(isobus::NAME::NAMEParameters::FunctionCode, static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal)) };
	auto ecu = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0);
	auto vt = isobus::CANNetworkManager::CANNetwork.create_partnered_control_function(0, vtFilters);
	auto client = std::make_shared<isobus::VirtualTerminalClient>(vt, ecu);
	client->set_object_pool(0, pool.data(), static_cast<std::uint32_t>(pool.size()), isobus::IOPFileInterface::hash_object_pool_to_version(pool));
	client->set_auxiliary_input_model_identification_code(1);
	client->add_auxiliary_input_object_id(AUXN_INPUT_SLIDER);
	client->add_auxiliary_input_object_id(AUXN_INPUT_BUTTON);
	client->initialize(true);
	emit("ready", { { "pool", poolPath }, { "bytes", pool.size() }, { "inputs", { { "button", AUXN_INPUT_BUTTON }, { "slider", AUXN_INPUT_SLIDER } } } });
	if (!joystickDevice.empty())
	{
		emit("joystick", { { "device", joystickDevice }, { "address", joystickAddress }, { "centre", { { "x", axisX.centre }, { "y", axisY.centre } } } });
	}

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

	bool lastConnected = false, lastLearn = false, buttonPressed = false;
	std::uint16_t transitions = 0;
	std::uint16_t slider = 0;
	bool joystickFailed = false;
	while (running)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		I2CJoystick::Reading reading;
		if (!joystickDevice.empty() && !joystick.read(reading))
		{
			if (!joystickFailed)
			{
				emit("joystick", { { "error", joystick.error() } });
			}
			joystickFailed = true;
		}
		else if (!joystickDevice.empty())
		{
			joystickFailed = false;
			if (reading != lastReading)
			{
				nlohmann::json codes;
				for (std::size_t b = 0; b < reading.codes.size(); b++)
				{
					codes[I2CJoystick::BUTTON_NAMES[b]] = reading.codes[b];
				}
				emit("joystick", { { "x", reading.x }, { "y", reading.y }, { "buttons", codes } });
				lastReading = reading;
			}
			const std::uint16_t x = axisX.scale(reading.x);
			if (x != slider)
			{
				slider = x;
				client->update_auxiliary_input(AUXN_INPUT_SLIDER, slider, 0xFFFF);
			}
			const bool a = reading.pressed(2); // BUTTON_NAMES[2], button A
			if (a != buttonPressed)
			{
				buttonPressed = a;
				transitions++;
				client->update_auxiliary_input(AUXN_INPUT_BUTTON, buttonPressed ? 1 : 0, transitions);
			}
		}
		const bool connected = client->get_is_connected();
		if (connected != lastConnected)
		{
			lastConnected = connected;
			emit("vt", { { "connected", connected } });
		}
		const bool learn = client->get_auxiliary_input_learn_mode_enabled();
		if (learn != lastLearn)
		{
			lastLearn = learn;
			emit("learn", { { "enabled", learn } });
		}
		std::vector<std::string> commands;
		{
			std::lock_guard<std::mutex> lock(commandMutex);
			commands.swap(pending);
		}
		for (const auto &line : commands)
		{
			std::string cmd, a1;
			std::size_t p = line.find(' ');
			cmd = line.substr(0, p);
			a1 = p == std::string::npos ? "" : line.substr(p + 1);
			bool ok = true;
			if (cmd == "quit")
			{
				running = false;
				break;
			}
			else if (cmd == "button")
			{
				const bool on = a1 == "on" || a1 == "1";
				if (on != buttonPressed)
				{
					transitions++;
				}
				buttonPressed = on;
				client->update_auxiliary_input(AUXN_INPUT_BUTTON, buttonPressed ? 1 : 0, transitions);
			}
			else if (cmd == "slider")
			{
				slider = static_cast<std::uint16_t>(std::min(64255L, std::max(0L, std::atol(a1.c_str()))));
				client->update_auxiliary_input(AUXN_INPUT_SLIDER, slider, 0xFFFF);
			}
			else
			{
				ok = false;
			}
			emit("command", { { "command", line }, { "ok", ok }, { "button", buttonPressed }, { "transitions", transitions }, { "slider", slider } });
		}
	}
	client->terminate();
	isobus::CANHardwareInterface::stop();
	std::fclose(stdin);
	if (reader.joinable())
	{
		reader.detach();
	}
	emit("exit");
	return 0;
}
