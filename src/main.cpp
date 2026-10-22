// implement: one simulated ISOBUS implement from a profile (docs/PLAN.md).
//
//   implement --profile profiles/sprayer-24.json [--interface vcan0] [--driver socketcan|virtual|peak]
//             [--port 0|<n>] [--pool <file.iop>] [--section-control setpoint|actualSettable|perElement]
//             [--log debug|info|warn|error]
//
// Events are JSON lines on stdout and to every client of the TCP port; commands are lines on
// stdin and from those clients; the stack's log goes to stderr.
#include "commands.hpp"
#include "events.hpp"
#include "fault_driver.hpp"
#include "model.hpp"
#include "profile.hpp"
#include "tc_client.hpp"
#include "vt_client.hpp"

#include "create_can_driver.hpp"
#include "isobus/isobus/can_stack_logger.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	std::atomic_bool running{ true };

	void on_signal(int)
	{
		running = false;
	}

	class StderrLogger : public isobus::CANStackLogger
	{
	public:
		void sink_CAN_stack_log(LoggingLevel level, const std::string &text) override
		{
			const char *name = "debug";
			switch (level)
			{
				case LoggingLevel::Info:
					name = "info";
					break;
				case LoggingLevel::Warning:
					name = "warn";
					break;
				case LoggingLevel::Error:
					name = "error";
					break;
				case LoggingLevel::Critical:
					name = "critical";
					break;
				default:
					break;
			}
			std::fprintf(stderr, "[stack %s] %s\n", name, text.c_str());
		}
	};

	void usage()
	{
		std::fprintf(stderr,
		             "usage: implement --profile <file> [--interface vcan0] [--driver socketcan|virtual|peak] [--port 0|<n>] [--pool <file.iop>]\n"
		             "                 [--section-control setpoint|actualSettable|perElement] [--log debug|info|warn|error] [--trace]\n");
	}
}

int main(int argc, char **argv)
{
	std::string profilePath, interface, driverName, sectionControl, poolOverride, logLevel = "warn";
	bool trace = false;
	int port = -1;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		const bool hasValue = i + 1 < argc;
		if (a == "--profile" && hasValue)
		{
			profilePath = argv[++i];
		}
		else if (a == "--interface" && hasValue)
		{
			interface = argv[++i];
		}
		else if (a == "--trace")
		{
			trace = true;
		}
		else if (a == "--driver" && hasValue)
		{
			driverName = argv[++i];
		}
		else if (a == "--section-control" && hasValue)
		{
			sectionControl = argv[++i];
		}
		else if (a == "--log" && hasValue)
		{
			logLevel = argv[++i];
		}
		else if (a == "--port" && hasValue)
		{
			port = std::atoi(argv[++i]);
		}
		else if (a == "--pool" && hasValue)
		{
			poolOverride = argv[++i];
		}
		else
		{
			usage();
			return 2;
		}
	}
	if (profilePath.empty())
	{
		usage();
		return 2;
	}

	sim::Profile profile;
	try
	{
		profile = sim::load_profile(profilePath);
		if (!poolOverride.empty())
		{
			profile.pool = poolOverride;
		}
		if (!sectionControl.empty() && !sim::section_control_from_string(sectionControl, profile.sectionControl))
		{
			std::fprintf(stderr, "--section-control must be setpoint, actualSettable or perElement\n");
			return 2;
		}
	}
	catch (const std::exception &e)
	{
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}

	StderrLogger logger;
	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	using L = isobus::CANStackLogger::LoggingLevel;
	isobus::CANStackLogger::set_log_level(logLevel == "debug" ? L::Debug : logLevel == "info" ? L::Info : logLevel == "error" ? L::Error : L::Warning);

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	auto realDriver = CANDriverFactory::create(interface, driverName);
	if (!realDriver)
	{
		return 1;
	}
	auto driver = std::make_shared<sim::FaultyDriver>(realDriver);
	driver->set_trace(trace);

	sim::Events events;
	sim::Model model(profile);
	std::mutex modelMutex;
	sim::ImplementClient client(profile, model, modelMutex, events);
	try
	{
		if (!client.start(driver))
		{
			std::fprintf(stderr, "could not open the CAN interface '%s'\n", interface.c_str());
			return 1;
		}
	}
	catch (const std::exception &e)
	{
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}

	// The terminal client, if the profile's pool is there
	const auto slash = profilePath.find_last_of('/');
	sim::TerminalClient terminal(profile, model, modelMutex, events, client);
	terminal.start(client.control_function(), slash == std::string::npos ? "." : profilePath.substr(0, slash));

	sim::CommandServer commands(profile, model, modelMutex, events, client, running);
	commands.set_terminal(&terminal);
	commands.set_fault_driver(driver);
	if (!commands.start(port))
	{
		std::fprintf(stderr, "could not listen on port %d\n", port);
		client.stop();
		return 1;
	}

	// The model ticks every 20 ms (MODEL.md section 8)
	auto next = std::chrono::steady_clock::now();
	while (running)
	{
		next += std::chrono::milliseconds(20);
		std::this_thread::sleep_until(next);
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			model.tick(events.now_ms());
		}
		client.update();
		terminal.update();
	}
	commands.stop();
	terminal.stop();
	client.stop();
	events.emit("exit");
	return 0;
}
