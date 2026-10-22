// Events out: one JSON object a line on stdout (docs/PLAN.md section 6). Stack logs go to stderr.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace sim
{
	class Events
	{
	public:
		Events();
		/// @brief Milliseconds since the program started; the "t" of every event.
		std::uint32_t now_ms() const;
		/// @brief Prints {"t":..,"event":name,...fields} as one line, to stdout and every sink.
		void emit(const std::string &name, nlohmann::json fields = nlohmann::json::object());
		/// @brief A connected socket that gets every event line from now on; dropped when a write fails.
		void add_sink(int fd);
		void remove_sink(int fd);

	private:
		std::uint64_t startNs;
		std::mutex mutex;
		std::set<int> sinks;
	};
}
