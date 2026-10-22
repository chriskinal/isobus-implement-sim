// Faults at the frame level (docs/PLAN.md section 7): a CAN driver around the real one that
// can go silent, delay every frame, or flood the bus with error frames.
#pragma once

#include "isobus/hardware_integration/can_hardware_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace sim
{
	class FaultyDriver : public isobus::CANHardwarePlugin
	{
	public:
		explicit FaultyDriver(std::shared_ptr<isobus::CANHardwarePlugin> real);

		std::string get_name() const override { return real->get_name(); }
		bool get_is_valid() const override { return real->get_is_valid(); }
		void close() override { real->close(); }
		void open() override { real->open(); }
		bool read_frame(isobus::CANMessageFrame &frame) override { return real->read_frame(frame); }
		bool write_frame(const isobus::CANMessageFrame &frame) override;

		/// @brief Drops every outgoing frame until the given steady-clock millisecond.
		void silent_until(std::uint64_t untilMs) { silentUntilMs = untilMs; }
		bool silent() const;
		/// @brief Holds every outgoing frame for this long before sending it. 0 clears it.
		void set_delay_ms(std::uint32_t ms) { delayMs = ms; }
		std::uint32_t delay_ms() const { return delayMs; }
		/// @brief Writes count error frames (bus-off reported) straight to the driver.
		std::uint32_t flood_error_frames(std::uint32_t count);
		std::uint64_t dropped() const { return droppedFrames; }

		static std::uint64_t now_ms();

	private:
		std::shared_ptr<isobus::CANHardwarePlugin> real;
		std::atomic<std::uint64_t> silentUntilMs{ 0 };
		std::atomic<std::uint32_t> delayMs{ 0 };
		std::atomic<std::uint64_t> droppedFrames{ 0 };
	};
}
