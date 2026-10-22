#include "fault_driver.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace sim
{
	FaultyDriver::FaultyDriver(std::shared_ptr<isobus::CANHardwarePlugin> r) :
	  real(std::move(r))
	{
	}

	std::uint64_t FaultyDriver::now_ms()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool FaultyDriver::silent() const
	{
		return now_ms() < silentUntilMs;
	}

	bool FaultyDriver::write_frame(const isobus::CANMessageFrame &frame)
	{
		if (silent())
		{
			droppedFrames++;
			return true; // the stack believes it went out, as a dead transceiver would
		}
		const std::uint32_t delay = delayMs;
		if (delay > 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(delay));
		}
		return real->write_frame(frame);
	}

	std::uint32_t FaultyDriver::flood_error_frames(std::uint32_t count)
	{
		// A SocketCAN error frame: the error flag in the identifier, bus-off in the class bits.
		// Receivers that ask for error frames see it; others, and other drivers, ignore it.
		isobus::CANMessageFrame frame;
		std::memset(&frame, 0, sizeof(frame));
		frame.identifier = 0x20000000u | 0x00000040u; // CAN_ERR_FLAG | CAN_ERR_BUSOFF
		frame.isExtendedFrame = false;
		frame.dataLength = 8;
		std::uint32_t sent = 0;
		for (std::uint32_t i = 0; i < count; i++)
		{
			if (real->write_frame(frame))
			{
				sent++;
			}
		}
		return sent;
	}
}
