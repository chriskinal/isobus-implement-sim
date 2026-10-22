#include "events.hpp"

#include <chrono>
#include <cstdio>

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
		const std::string text = line.dump();
		std::lock_guard<std::mutex> lock(mutex);
		std::fputs(text.c_str(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}
}
