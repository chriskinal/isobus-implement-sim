// aux_input: a simulated auxiliary input device (docs/PLAN.md phase 7), after AgIsoStack++
// examples/virtual_terminal/aux_inputs (MIT). It uploads its pool to the terminal and sends the
// input states a terminal has assigned to functions. Two pools: pools/aux_inputs.iop, the stack
// example's slider and button (the default), and pools/joystick.iop, the joystick module's two
// spring-centred axes (x, y) and five buttons (a, b, c, d, ok). Commands on stdin:
//
//   button on|off              the first button, with its transition count
//   button <name> on|off       a button by name
//   slider <0..64255>          the first analogue input
//   axis <name> <0..64255>     an analogue input by name
//   quit
//
//   aux_input [--pool pools/aux_inputs.iop|pools/joystick.iop] [--interface vcan0] [--driver socketcan|virtual|aowlink]
//             [--joystick /dev/i2c-1 | --joystick-serial /dev/ttyUSB0 | --joystick-tcp <host[:port]>]
//             [--joystick-address 0x5a]
//
// With --joystick (Linux), the I2C joystick module of tools/i2c_joystick.hpp drives the inputs
// too: with the joystick pool every axis and button, with the example pool its X axis the slider
// and its A button the button. --joystick-serial reads the same module
// through an ESP32 running tools/i2c_bridge, for a machine with no free I2C bus; --joystick-tcp
// through SharpIsoTerm's ESP32 AOW link gateway over the network (its port 2324), so with
// --driver aowlink --interface <the gateway> nothing is wired to this machine. Every change of
// what the module reports, the other axis and buttons included, prints a "joystick" event with
// the raw values.
#include "../pools/aux_inputs_ids.hpp"
#include "../pools/joystick_ids.hpp"
#include "can_driver.hpp"
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
#include <sstream>
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
	bool joystickSerial = false, joystickTcp = false;
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
		else if (a == "--joystick-serial" && i + 1 < argc)
		{
			joystickDevice = argv[++i];
			joystickSerial = true;
		}
		else if (a == "--joystick-tcp" && i + 1 < argc)
		{
			joystickDevice = argv[++i];
			joystickTcp = true;
		}
		else if (a == "--joystick-address" && i + 1 < argc)
		{
			joystickAddress = std::strtol(argv[++i], nullptr, 0);
		}
		else
		{
			std::fprintf(stderr, "usage: aux_input [--pool pools/aux_inputs.iop] [--interface vcan0] [--driver socketcan|virtual|aowlink] [--joystick /dev/i2c-1 | --joystick-serial /dev/ttyUSB0 | --joystick-tcp host[:port]] [--joystick-address 0x5a]\n");
			return 2;
		}
	}
	StderrLogger logger;
	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Warning);
	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);

	auto pool = isobus::IOPFileInterface::read_iop_file(poolPath);
	const bool joystickPool = poolPath.size() >= 12 && poolPath.compare(poolPath.size() - 12, 12, "joystick.iop") == 0;
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
		const auto address = static_cast<std::uint8_t>(joystickAddress);
		if (!(joystickTcp ? joystick.open_tcp(joystickDevice, address) : joystickSerial ? joystick.open_serial(joystickDevice, address) : joystick.open(joystickDevice, address)))
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

	auto driver = sim::create_can_driver(interface, driverName, "aux_input");
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
	// The joystick pool is another device than the example's, with its own identity: a terminal
	// keeps a pool by NAME, and the stack's VT server would add the new pool's working set to the
	// old one's under the same NAME and refuse it
	name.set_identity_number(joystickPool ? 903 : 902);
	name.set_manufacturer_code(1407);
	const std::vector<isobus::NAMEFilter> vtFilters = { isobus::NAMEFilter(isobus::NAME::NAMEParameters::FunctionCode, static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal)) };
	auto ecu = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0);
	auto vt = isobus::CANNetworkManager::CANNetwork.create_partnered_control_function(0, vtFilters);
	auto client = std::make_shared<isobus::VirtualTerminalClient>(vt, ecu);
	client->set_object_pool(0, pool.data(), static_cast<std::uint32_t>(pool.size()), isobus::IOPFileInterface::hash_object_pool_to_version(pool));
	// The inputs the pool has: the stack example's slider and button, or the joystick pool's
	// two axes and five buttons. The first analogue and the first boolean are what "slider" and
	// "button" without a name move, as before.
	struct Input
	{
		std::string name;
		std::uint16_t id;
		bool analog;
		std::uint16_t value = 0; // analogue: 0 to 64255; boolean: 0 released, 1 pressed
		std::uint16_t transitions = 0; // boolean only
	};
	std::vector<Input> inputs;
	if (joystickPool)
	{
		constexpr std::uint16_t middle = JoystickAxis::MAX / 2; // they spring back to 50 %
		inputs = { { "x", joystick_pool::stickX_AuxInput, true, middle },
		           { "y", joystick_pool::stickY_AuxInput, true, middle },
		           { "a", joystick_pool::buttonA_AuxInput, false },
		           { "b", joystick_pool::buttonB_AuxInput, false },
		           { "c", joystick_pool::buttonC_AuxInput, false },
		           { "d", joystick_pool::buttonD_AuxInput, false },
		           { "ok", joystick_pool::buttonOk_AuxInput, false } };
	}
	else
	{
		inputs = { { "slider", AUXN_INPUT_SLIDER, true }, { "button", AUXN_INPUT_BUTTON, false } };
	}
	auto find_input = [&](const std::string &inputName, bool analog) -> Input * {
		for (auto &input : inputs)
		{
			if (input.analog == analog && (inputName.empty() || input.name == inputName))
			{
				return &input;
			}
		}
		return nullptr;
	};
	auto set_analog = [&](Input &input, std::uint16_t value) {
		input.value = std::min<std::uint16_t>(value, JoystickAxis::MAX);
		client->update_auxiliary_input(input.id, input.value, 0xFFFF);
	};
	auto set_button = [&](Input &input, bool pressed) {
		if (pressed != (input.value != 0))
		{
			input.transitions++;
		}
		input.value = pressed ? 1 : 0;
		client->update_auxiliary_input(input.id, input.value, input.transitions);
	};

	// A different pool is a different input device to the terminal, so its assignments are kept
	// apart from the example pool's
	client->set_auxiliary_input_model_identification_code(joystickPool ? 2 : 1);
	nlohmann::json ids = nlohmann::json::object();
	for (const auto &input : inputs)
	{
		client->add_auxiliary_input_object_id(input.id);
		ids[input.name] = input.id;
	}
	client->initialize(true);
	emit("ready", { { "pool", poolPath }, { "bytes", pool.size() }, { "inputs", ids } });
	if (!joystickDevice.empty())
	{
		emit("joystick", { { "device", joystickDevice }, { "bridge", joystickTcp ? "tcp" : joystickSerial ? "serial" : "none" }, { "address", joystickAddress }, { "centre", { { "x", axisX.centre }, { "y", axisY.centre } } } });
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

	bool lastConnected = false, lastLearn = false;
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
			// The joystick pool: every axis and button to its input. The example pool: X to the
			// slider and A to the button.
			const std::uint16_t x = axisX.scale(reading.x), y = axisY.scale(reading.y);
			Input *ax = find_input(joystickPool ? "x" : "", true);
			if (ax && x != ax->value)
			{
				set_analog(*ax, x);
			}
			Input *ay = joystickPool ? find_input("y", true) : nullptr;
			if (ay && y != ay->value)
			{
				set_analog(*ay, y);
			}
			for (std::size_t b = 0; b < reading.codes.size(); b++)
			{
				const std::string button = I2CJoystick::BUTTON_NAMES[b];
				Input *input = joystickPool ? find_input(button, false) : (button == "a" ? find_input("", false) : nullptr);
				if (input && reading.pressed(b) != (input->value != 0))
				{
					set_button(*input, reading.pressed(b));
				}
			}
		}
		const bool connected = client->get_is_connected();
		if (connected != lastConnected)
		{
			lastConnected = connected;
			emit("vt", { { "connected", connected } });
			// The stack keeps its own copy of each input, from 0, and takes updates only while
			// connected. On an axis 0 is full deflection, and a stick left at rest never moves
			// off the middle to send another, so every input's value goes over on connecting.
			for (const auto &input : inputs)
			{
				if (connected)
				{
					client->update_auxiliary_input(input.id, input.value, input.analog ? 0xFFFF : input.transitions);
				}
			}
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
			std::istringstream words(line);
			std::string cmd, a1, a2;
			words >> cmd >> a1 >> a2;
			bool ok = true;
			if (cmd == "quit")
			{
				running = false;
				break;
			}
			else if (cmd == "button")
			{
				// button on|off, or button <name> on|off
				const std::string state = a2.empty() ? a1 : a2;
				Input *input = find_input(a2.empty() ? "" : a1, false);
				ok = input && (state == "on" || state == "off" || state == "1" || state == "0");
				if (ok)
				{
					set_button(*input, state == "on" || state == "1");
				}
			}
			else if (cmd == "slider" || cmd == "axis")
			{
				// slider <0..64255>, or axis <name> <0..64255>
				const std::string value = cmd == "slider" ? a1 : a2;
				Input *input = find_input(cmd == "slider" ? "" : a1, true);
				ok = input && !value.empty();
				if (ok)
				{
					set_analog(*input, static_cast<std::uint16_t>(std::min(64255L, std::max(0L, std::atol(value.c_str())))));
				}
			}
			else
			{
				ok = false;
			}
			nlohmann::json values = nlohmann::json::object();
			for (const auto &input : inputs)
			{
				values[input.name] = input.analog ? nlohmann::json(input.value) : nlohmann::json(input.value != 0);
			}
			const Input *firstButton = find_input("", false);
			const Input *firstAxis = find_input("", true);
			emit("command", { { "command", line }, { "ok", ok }, { "button", firstButton->value != 0 }, { "transitions", firstButton->transitions }, { "slider", firstAxis->value }, { "inputs", values } });
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
