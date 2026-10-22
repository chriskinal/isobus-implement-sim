#include "commands.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace sim
{
	namespace
	{
		std::vector<std::string> split(const std::string &line)
		{
			std::istringstream in(line);
			std::vector<std::string> words;
			std::string w;
			while (in >> w)
			{
				words.push_back(w);
			}
			return words;
		}

		bool on_off(const std::string &w, bool &out)
		{
			if (w == "on" || w == "1" || w == "true")
			{
				out = true;
				return true;
			}
			if (w == "off" || w == "0" || w == "false")
			{
				out = false;
				return true;
			}
			return false;
		}

		bool number(const std::string &w, double &out)
		{
			char *end = nullptr;
			out = std::strtod(w.c_str(), &end);
			return end != w.c_str() && *end == '\0';
		}

		const char *HELP =
		  "switch <section> on|off | master on|off [boom] | auto on|off [boom] | rate <value> [product] | "
		  "bin <level> [product] | tank <level> | fill [product] | depth <mm> | speed <km/h>|bus | "
		  "fault row|nozzle <section> blocked|clear | fault silent <s> | fault restart | fault resume | "
		  "fault version <label> | fault bigpool <kB> | fault slow <ms> | fault conflict [address] | fault ddop bad | "
		  "fault busoff [frames] | alarm <name>|clear | key <object> | mask <id> | value <object> <n> | status | help | quit";
	}

	CommandServer::CommandServer(const Profile &p, Model &m, std::mutex &mx, Events &ev, ImplementClient &c, std::atomic_bool &run) :
	  profile(p),
	  model(m),
	  modelMutex(mx),
	  events(ev),
	  client(c),
	  running(run)
	{
	}

	CommandServer::~CommandServer()
	{
		stop();
	}

	bool CommandServer::start(int port)
	{
		if (port >= 0)
		{
			listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
			if (listenFd < 0)
			{
				return false;
			}
			int one = 1;
			::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.sin_port = htons(static_cast<std::uint16_t>(port));
			if (::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(listenFd, 4) < 0)
			{
				::close(listenFd);
				listenFd = -1;
				return false;
			}
			socklen_t len = sizeof(addr);
			::getsockname(listenFd, reinterpret_cast<sockaddr *>(&addr), &len);
			listenPort = ntohs(addr.sin_port);
		}
		if (::pipe(wakePipe) != 0)
		{
			return false;
		}
		ioRunning = true;
		thread = std::thread([this] { io_loop(); });
		events.emit("listening", { { "port", listenPort }, { "stdin", true } });
		return true;
	}

	void CommandServer::stop()
	{
		if (!ioRunning)
		{
			return;
		}
		ioRunning = false;
		if (wakePipe[1] >= 0)
		{
			const char c = 'x';
			(void)::write(wakePipe[1], &c, 1);
		}
		if (thread.joinable())
		{
			thread.join();
		}
		for (auto &c : clients)
		{
			events.remove_sink(c.first);
			::close(c.first);
		}
		clients.clear();
		if (listenFd >= 0)
		{
			::close(listenFd);
			listenFd = -1;
		}
		for (int &fd : wakePipe)
		{
			if (fd >= 0)
			{
				::close(fd);
				fd = -1;
			}
		}
	}

	void CommandServer::io_loop()
	{
		std::string stdinLine;
		bool stdinOpen = true;
		while (ioRunning)
		{
			std::vector<pollfd> fds;
			fds.push_back({ wakePipe[0], POLLIN, 0 });
			if (stdinOpen)
			{
				fds.push_back({ STDIN_FILENO, POLLIN, 0 });
			}
			if (listenFd >= 0)
			{
				fds.push_back({ listenFd, POLLIN, 0 });
			}
			for (const auto &c : clients)
			{
				fds.push_back({ c.first, POLLIN, 0 });
			}
			if (::poll(fds.data(), fds.size(), 500) <= 0)
			{
				continue;
			}
			for (const auto &p : fds)
			{
				if (!(p.revents & (POLLIN | POLLHUP | POLLERR)))
				{
					continue;
				}
				if (p.fd == wakePipe[0])
				{
					char buf[8];
					(void)::read(wakePipe[0], buf, sizeof(buf));
				}
				else if (p.fd == STDIN_FILENO)
				{
					char buf[512];
					const auto n = ::read(STDIN_FILENO, buf, sizeof(buf));
					if (n <= 0)
					{
						stdinOpen = false; // EOF: a pipe closed, or no stdin at all
						continue;
					}
					stdinLine.append(buf, static_cast<std::size_t>(n));
					std::size_t nl;
					while ((nl = stdinLine.find('\n')) != std::string::npos)
					{
						execute(stdinLine.substr(0, nl), "stdin");
						stdinLine.erase(0, nl + 1);
					}
				}
				else if (p.fd == listenFd)
				{
					const int fd = ::accept(listenFd, nullptr, nullptr);
					if (fd >= 0)
					{
						clients[fd] = "";
						events.add_sink(fd);
						events.emit("client", { { "connected", true }, { "clients", clients.size() } });
					}
				}
				else
				{
					char buf[512];
					const auto n = ::recv(p.fd, buf, sizeof(buf), 0);
					if (n <= 0)
					{
						events.remove_sink(p.fd);
						::close(p.fd);
						clients.erase(p.fd);
						events.emit("client", { { "connected", false }, { "clients", clients.size() } });
						continue;
					}
					auto &line = clients[p.fd];
					line.append(buf, static_cast<std::size_t>(n));
					std::size_t nl;
					while ((nl = line.find('\n')) != std::string::npos)
					{
						execute(line.substr(0, nl), "tcp");
						line.erase(0, nl + 1);
					}
				}
			}
		}
	}

	bool CommandServer::section_index(const std::string &text, std::size_t &boom, std::size_t &index, std::string &error) const
	{
		double n;
		if (!number(text, n) || n < 1 || n != static_cast<int>(n))
		{
			error = "section must be a number from 1";
			return false;
		}
		std::size_t rest = static_cast<std::size_t>(n) - 1;
		for (boom = 0; boom < profile.booms.size(); boom++)
		{
			if (rest < profile.booms[boom].sections.size())
			{
				index = rest;
				return true;
			}
			rest -= profile.booms[boom].sections.size();
		}
		error = "no section " + text + ": the machine has " + std::to_string(profile.total_sections());
		return false;
	}

	bool CommandServer::product_index(const std::string &text, std::size_t &product, std::string &error) const
	{
		if (profile.products.empty())
		{
			error = "the machine has no product";
			return false;
		}
		if (text.empty())
		{
			product = 0;
			return true;
		}
		double n;
		if (number(text, n) && n >= 1 && n <= profile.products.size())
		{
			product = static_cast<std::size_t>(n) - 1;
			return true;
		}
		for (std::size_t i = 0; i < profile.products.size(); i++)
		{
			if (profile.products[i].name == text)
			{
				product = i;
				return true;
			}
		}
		error = "no product " + text;
		return false;
	}

	nlohmann::json CommandServer::status()
	{
		nlohmann::json s;
		s["profile"] = profile.name;
		s["type"] = profile.type;
		s["tc"] = { { "connected", client.connected() }, { "state", client.state() } };
		s["address"] = client.address();
		if (terminal && terminal->active())
		{
			s["vt"] = { { "connected", terminal->connected() }, { "mask", terminal->active_mask() }, { "pool", terminal->pool_path() } };
		}
		else
		{
			s["vt"] = { { "connected", false }, { "pool", nullptr } };
		}
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			s["booms"] = nlohmann::json::array();
			for (std::size_t b = 0; b < model.boom_count(); b++)
			{
				const auto &bs = model.boom(b);
				s["booms"].push_back({ { "auto", bs.autoMode }, { "master", bs.master }, { "setpoint", model.setpoint_string(b) }, { "commanded", model.commanded_string(b) }, { "actual", model.actual_string(b) }, { "areaM2", bs.areaM2 }, { "effectiveTimeS", bs.effectiveTimeS } });
			}
			s["products"] = nlohmann::json::array();
			for (std::size_t p = 0; p < model.product_count(); p++)
			{
				const auto &ps = model.product(p);
				nlohmann::json pj = { { "name", profile.products[p].name }, { "unit", profile.products[p].unit }, { "target", model.target_rate(p) }, { "actual", ps.actualRate }, { "level", ps.level }, { "binUnit", profile.products[p].bin.unit }, { "tripTotal", ps.tripTotal }, { "prescription", ps.prescription } };
				if (profile.type == "sprayer")
				{
					pj["flowLPerMin"] = ps.flowLPerMin;
					pj["pressureBar"] = ps.pressureBar;
				}
				s["products"].push_back(pj);
			}
			s["speed"] = { { "known", model.speed_known() }, { "kmh", model.speed_mm_s() * 0.0036 }, { "source", model.fixed_speed() ? "fixed" : "bus" } };
			if (profile.type == "seeder")
			{
				s["depthMm"] = model.depth();
			}
		}
		return s;
	}

	void CommandServer::execute(const std::string &rawLine, const std::string &from)
	{
		std::lock_guard<std::mutex> lock(executeMutex);
		std::string line = rawLine;
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		const auto words = split(line);
		nlohmann::json result = { { "command", line }, { "from", from }, { "ok", true } };
		auto fail = [&](const std::string &why) {
			result["ok"] = false;
			result["error"] = why;
		};
		if (words.empty())
		{
			return;
		}
		const std::string &cmd = words[0];
		const std::string a1 = words.size() > 1 ? words[1] : "";
		const std::string a2 = words.size() > 2 ? words[2] : "";
		const std::string a3 = words.size() > 3 ? words[3] : "";
		bool flag = false;
		double value = 0;
		std::size_t boom = 0, index = 0, product = 0;
		std::string error;

		if (cmd == "help")
		{
			result["help"] = HELP;
		}
		else if (cmd == "quit")
		{
			running = false;
		}
		else if (cmd == "status")
		{
			result["status"] = status();
		}
		else if (cmd == "switch")
		{
			if (!section_index(a1, boom, index, error))
			{
				fail(error);
			}
			else if (!on_off(a2, flag))
			{
				fail("switch <section> on|off");
			}
			else
			{
				std::lock_guard<std::mutex> m(modelMutex);
				model.set_switch(boom, index, flag);
				result["boom"] = boom;
				result["section"] = index + 1;
			}
		}
		else if (cmd == "master" || cmd == "auto")
		{
			if (!on_off(a1, flag))
			{
				fail(cmd + " on|off [boom]");
			}
			else
			{
				double b = 0;
				if (!a2.empty() && (!number(a2, b) || b < 1 || b > profile.booms.size()))
				{
					fail("no boom " + a2);
				}
				else
				{
					std::lock_guard<std::mutex> m(modelMutex);
					for (std::size_t i = 0; i < profile.booms.size(); i++)
					{
						if (a2.empty() || i == static_cast<std::size_t>(b) - 1)
						{
							if (cmd == "master")
							{
								model.set_master(i, flag);
							}
							else
							{
								model.set_auto(i, flag);
							}
						}
					}
				}
			}
		}
		else if (cmd == "rate")
		{
			if (!number(a1, value))
			{
				fail("rate <value> [product]");
			}
			else if (!product_index(a2, product, error))
			{
				fail(error);
			}
			else
			{
				std::lock_guard<std::mutex> m(modelMutex);
				model.set_operator_rate(product, value);
				result["product"] = profile.products[product].name;
				result["rate"] = model.product(product).operatorRate;
				result["inEffect"] = !model.product(product).prescription;
				if (model.product(product).prescription)
				{
					result["note"] = "prescription control is on: the task controller's setpoint rules until it sends DDI 158 = 0";
				}
			}
		}
		else if (cmd == "bin" || cmd == "tank")
		{
			if (!number(a1, value))
			{
				fail(cmd + " <level> [product]");
			}
			else if (!product_index(a2, product, error))
			{
				fail(error);
			}
			else
			{
				std::lock_guard<std::mutex> m(modelMutex);
				model.set_bin_level(product, value);
				result["product"] = profile.products[product].name;
				result["level"] = model.product(product).level;
			}
		}
		else if (cmd == "fill")
		{
			if (!product_index(a1, product, error))
			{
				fail(error);
			}
			else
			{
				std::lock_guard<std::mutex> m(modelMutex);
				model.set_bin_level(product, profile.products[product].bin.capacity);
				result["product"] = profile.products[product].name;
				result["level"] = model.product(product).level;
			}
		}
		else if (cmd == "depth")
		{
			if (profile.type != "seeder" || profile.seeder.kind != "planter")
			{
				fail("depth is for planters");
			}
			else if (!number(a1, value))
			{
				fail("depth <mm>");
			}
			else
			{
				std::lock_guard<std::mutex> m(modelMutex);
				model.set_depth(static_cast<std::int32_t>(value));
				result["depthMm"] = model.depth();
			}
		}
		else if (cmd == "speed")
		{
			if (a1 == "bus")
			{
				std::lock_guard<std::mutex> m(modelMutex);
				model.clear_fixed_speed();
				result["source"] = "bus";
			}
			else if (!number(a1, value) || value < 0)
			{
				fail("speed <km/h> | speed bus");
			}
			else
			{
				std::lock_guard<std::mutex> m(modelMutex);
				model.set_fixed_speed(value / 3.6 * 1000.0);
				result["source"] = "fixed";
				result["kmh"] = value;
			}
		}
		else if (cmd == "alarm" || cmd == "key" || cmd == "mask" || cmd == "value")
		{
			if (!terminal || !terminal->active())
			{
				fail("no terminal client: the profile's pool did not load");
			}
			else if (cmd == "alarm")
			{
				if (a1 == "clear")
				{
					terminal->clear_alarms();
				}
				else if (a1.empty() || !terminal->raise_alarm(a1, error))
				{
					fail(error.empty() ? "alarm <name> | alarm clear" : error);
				}
			}
			else if (cmd == "key")
			{
				if (!number(a1, value) || value < 0 || value > 65535)
				{
					fail("key <object id>");
				}
				else
				{
					terminal->press_key(static_cast<std::uint16_t>(value));
				}
			}
			else if (cmd == "mask")
			{
				if (!number(a1, value) || value < 0 || value > 65535)
				{
					fail("mask <object id>");
				}
				else if (!terminal->change_mask(static_cast<std::uint16_t>(value)))
				{
					fail("the terminal is not connected");
				}
			}
			else
			{
				double v = 0;
				if (!number(a1, value) || value < 0 || value > 65535 || !number(a2, v) || v < 0)
				{
					fail("value <object id> <n>");
				}
				else if (!terminal->set_value(static_cast<std::uint16_t>(value), static_cast<std::uint32_t>(v)))
				{
					fail("the terminal is not connected");
				}
			}
		}
		else if (cmd == "fault")
		{
			if ((a1 == "row" || a1 == "nozzle") && section_index(a2, boom, index, error))
			{
				if (a3 == "blocked" || a3 == "clear")
				{
					std::lock_guard<std::mutex> m(modelMutex);
					model.set_section_error(boom, index, a3 == "blocked");
					result["boom"] = boom;
					result["section"] = index + 1;
					result["blocked"] = a3 == "blocked";
				}
				else
				{
					fail("fault " + a1 + " <section> blocked|clear");
				}
			}
			else if (a1 == "row" || a1 == "nozzle")
			{
				fail(error);
			}
			else if (a1 == "silent")
			{
				if (!faults || !number(a2, value) || value <= 0)
				{
					fail("fault silent <seconds>");
				}
				else
				{
					faults->silent_until(FaultyDriver::now_ms() + static_cast<std::uint64_t>(value * 1000.0));
					result["seconds"] = value;
				}
			}
			else if (a1 == "slow")
			{
				if (!faults || !number(a2, value) || value < 0)
				{
					fail("fault slow <ms>, 0 to clear");
				}
				else
				{
					faults->set_delay_ms(static_cast<std::uint32_t>(value));
					result["ms"] = static_cast<std::uint32_t>(value);
				}
			}
			else if (a1 == "busoff")
			{
				const std::uint32_t count = (number(a2, value) && value > 0) ? static_cast<std::uint32_t>(value) : 100;
				result["sent"] = faults ? faults->flood_error_frames(count) : 0;
			}
			else if (a1 == "restart")
			{
				// The terminal client stops first: it must not be using the control function that goes away
				const bool hadTerminal = terminal && terminal->active();
				if (hadTerminal)
				{
					terminal->leave();
				}
				auto ecu = client.restart();
				if (hadTerminal)
				{
					terminal->rejoin(ecu);
				}
			}
			else if (a1 == "resume")
			{
				fail("resume is a terminal restart the implement does not notice: restart the terminal within its timeout instead (PLAN.md section 7)");
			}
			else if (a1 == "version" || a1 == "bigpool")
			{
				if (!terminal || !terminal->active())
				{
					fail("no terminal client: the profile's pool did not load");
				}
				else if (a1 == "version" && (a2.empty() || a2.size() > 7))
				{
					fail("fault version <label of up to 7 characters>");
				}
				else if (a1 == "bigpool" && (!number(a2, value) || value <= 0))
				{
					fail("fault bigpool <kB>");
				}
				else
				{
					terminal->reupload(a1 == "version" ? a2 : "", a1 == "bigpool" ? static_cast<std::size_t>(value * 1024.0) : 0);
					result["label"] = a1 == "version" ? a2 : "hash";
					if (a1 == "bigpool")
					{
						result["padBytes"] = static_cast<std::size_t>(value * 1024.0);
						result["note"] = "padding is capped near 259 kB by the object ID range";
					}
				}
			}
			else if (a1 == "conflict")
			{
				const std::uint8_t address = (number(a2, value) && value > 0 && value < 254) ? static_cast<std::uint8_t>(value) : 0;
				if (!client.claim_conflict(address))
				{
					fail("no address to claim: the task controller is not known and none was given");
				}
			}
			else if (a1 == "ddop")
			{
				if (!client.reupload_bad_ddop())
				{
					fail("not connected to a task controller");
				}
			}
			else
			{
				fail("unknown fault; " + std::string(HELP));
			}
		}
		else
		{
			fail("unknown command; " + std::string(HELP));
		}
		events.emit("command", result);
	}
}
