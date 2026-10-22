// Commands in: one line each on stdin and on a local TCP port (docs/PLAN.md section 6). Every
// command is answered with a "command" event, and a TCP client gets every event from then on.
#pragma once

#include "events.hpp"
#include "model.hpp"
#include "profile.hpp"
#include "tc_client.hpp"
#include "vt_client.hpp"
#include "fault_driver.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace sim
{
	class CommandServer
	{
	public:
		CommandServer(const Profile &profile, Model &model, std::mutex &modelMutex, Events &events, ImplementClient &client, std::atomic_bool &running);
		~CommandServer();

		/// @brief Starts reading stdin, and listens on 127.0.0.1:port (0 for any free port, -1 for none).
		/// Emits a "listening" event with the port. False if the port cannot be opened.
		bool start(int port);
		void stop();
		int port() const { return listenPort; }

		/// @brief Runs one command line. Thread-safe. The result is the "command" event.
		void execute(const std::string &line, const std::string &from);
		void set_terminal(TerminalClient *t) { terminal = t; }
		void set_fault_driver(std::shared_ptr<FaultyDriver> d) { faults = std::move(d); }

	private:
		void io_loop();
		nlohmann::json status();
		bool section_index(const std::string &text, std::size_t &boom, std::size_t &index, std::string &error) const;
		bool product_index(const std::string &text, std::size_t &product, std::string &error) const;

		const Profile &profile;
		Model &model;
		std::mutex &modelMutex;
		Events &events;
		ImplementClient &client;
		TerminalClient *terminal = nullptr;
		std::shared_ptr<FaultyDriver> faults;
		std::atomic_bool &running;
		std::mutex executeMutex;
		std::thread thread;
		std::atomic_bool ioRunning{ false };
		int listenFd = -1;
		int listenPort = -1;
		int wakePipe[2] = { -1, -1 };
		std::map<int, std::string> clients; ///< fd to its partial line
	};
}
