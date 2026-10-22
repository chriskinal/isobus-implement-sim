#include "events.hpp"

#include <chrono>
#include <cstdio>
#include <sys/socket.h>
#include <vector>

namespace sim
{
	namespace
	{
		std::uint64_t steady_ns()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}
	}

	Events::Events() :
	  startNs(steady_ns())
	{
	}

	std::uint32_t Events::now_ms() const
	{
		return static_cast<std::uint32_t>((steady_ns() - startNs) / 1000000ULL);
	}

	void Events::emit(const std::string &name, nlohmann::json fields)
	{
		nlohmann::json line = nlohmann::json::object();
		line["t"] = now_ms();
		line["event"] = name;
		for (auto &item : fields.items())
		{
			line[item.key()] = item.value();
		}
		const std::string text = line.dump() + "\n";
		std::lock_guard<std::mutex> lock(mutex);
		std::fputs(text.c_str(), stdout);
		std::fflush(stdout);
		std::vector<int> dead;
		for (int fd : sinks)
		{
			std::size_t sent = 0;
			while (sent < text.size())
			{
				const auto n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
				if (n <= 0)
				{
					dead.push_back(fd);
					break;
				}
				sent += static_cast<std::size_t>(n);
			}
		}
		for (int fd : dead)
		{
			sinks.erase(fd);
		}
	}

	void Events::add_sink(int fd)
	{
		std::lock_guard<std::mutex> lock(mutex);
		sinks.insert(fd);
	}

	void Events::remove_sink(int fd)
	{
		std::lock_guard<std::mutex> lock(mutex);
		sinks.erase(fd);
	}
}
