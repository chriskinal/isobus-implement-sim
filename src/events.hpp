// Events out: one JSON object a line on stdout (docs/PLAN.md section 6). Stack logs go to stderr.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>

namespace sim
{
	class Events
	{
	public:
		Events();
		/// @brief Milliseconds since the program started; the "t" of every event.
		std::uint32_t now_ms() const;
		/// @brief Prints {"t":..,"event":name,...fields} as one line.
		void emit(const std::string &name, nlohmann::json fields = nlohmann::json::object());

	private:
		std::uint64_t startNs;
		std::mutex mutex;
	};
}
