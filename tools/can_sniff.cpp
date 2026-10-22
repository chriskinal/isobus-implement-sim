// can_sniff: candump for the stack's drivers, for a Mac with a PEAK adapter or any machine
// without can-utils. Prints every frame the driver receives or sends, with the PGN, the source
// and destination addresses and the name of the PGNs an implement meets, and once a second a
// line with the counts; with the MacCAN driver, the controller's bus state too, which is how a
// bus with no other node (no acknowledgement) or a wrong bitrate shows itself.
//
//   can_sniff [--driver peak|socketcan|virtual] [--interface 1|can0] [--seconds 10] [--probe]
//
// --probe sends a request for address claim to every node (PGN EA00 asking for EE00, from the
// null address) after the first second: a node on the bus must answer with its address claim,
// and an adapter alone on the wire never gets the frame acknowledged, which the bus state shows.
//
// MIT, part of isobus-implement-sim.

#include "isobus/hardware_integration/available_can_drivers.hpp"
#include "isobus/hardware_integration/can_hardware_interface.hpp"

#include "create_can_driver.hpp"

#ifdef ISOBUS_MACCANPCAN_AVAILABLE
#include "isobus/hardware_integration/PCBUSB.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	std::mutex printMutex;
	std::atomic<unsigned> rxCount{ 0 };
	std::atomic<unsigned> txCount{ 0 };
	std::map<std::uint8_t, unsigned> rxBySource;
	std::chrono::steady_clock::time_point started;

	const char *pgn_name(std::uint32_t pgn)
	{
		switch (pgn)
		{
			case 0xEE00: return "AddressClaim";
			case 0xEA00: return "Request";
			case 0xE800: return "Acknowledge";
			case 0xEC00: return "TP.CM";
			case 0xEB00: return "TP.DT";
			case 0xC800: return "ETP.CM";
			case 0xC700: return "ETP.DT";
			case 0xE700: return "ECU->VT";
			case 0xE600: return "VT->ECU";
			case 0xCB00: return "ProcessData";
			case 0xEF00: return "ProprietaryA";
			case 0xFEF1: return "WheelSpeed(CCVS)";
			case 0xFE48: return "WheelBasedSpeed";
			case 0xFE49: return "GroundBasedSpeed";
			case 0xFE4A: return "MachineSelectedSpeed";
			case 0xFECA: return "DM1";
			case 0xFED8: return "LanguageCommand";
			case 0xFDA3: return "WorkingSetMaster";
			case 0xFE0D: return "WorkingSetMember";
			case 0xFE06: return "VTStatus(legacy)";
			default: return (pgn >= 0xFF00) ? "ProprietaryB" : "";
		}
	}

	void print_frame(const char *direction, const isobus::CANMessageFrame &frame)
	{
		const std::uint32_t id = frame.identifier;
		const std::uint8_t pf = static_cast<std::uint8_t>(id >> 16);
		const std::uint8_t ps = static_cast<std::uint8_t>(id >> 8);
		const std::uint8_t src = static_cast<std::uint8_t>(id);
		const std::uint32_t pgn = (pf < 240) ? ((id >> 8) & 0x3FF00u) : ((id >> 8) & 0x3FFFFu);
		const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

		std::lock_guard<std::mutex> lock(printMutex);
		if (0 == std::strcmp(direction, "rx"))
		{
			rxBySource[src]++;
		}
		std::printf("%8.3f %s %08X [%u]", t, direction, id, frame.dataLength);
		for (std::uint8_t i = 0; i < frame.dataLength && i < 8; i++)
		{
			std::printf(" %02X", frame.data[i]);
		}
		for (std::uint8_t i = frame.dataLength; i < 8; i++)
		{
			std::printf("   ");
		}
		if (!frame.isExtendedFrame && 0 == id)
		{
			// The MacCAN driver's status frame (MSGTYPE 0x80), when status frames are allowed: the
			// bus state in the fourth byte (04 light, 08 heavy, 10 bus-off, 40 passive)
			std::printf("  STATUS frame: bus state 0x%02X\n", frame.dataLength > 3 ? frame.data[3] : 0);
			std::fflush(stdout);
			return;
		}
		std::printf("  PGN %05X src %02X", pgn, src);
		if (pf < 240)
		{
			std::printf(" dst %02X", ps);
		}
		else
		{
			std::printf("       ");
		}
		std::printf("  %s\n", pgn_name(pgn));
		std::fflush(stdout);
	}

	std::string bus_state(int channel)
	{
#ifdef ISOBUS_MACCANPCAN_AVAILABLE
		const TPCANStatus status = CAN_GetStatus(static_cast<TPCANHandle>(PCAN_USBBUS1 + channel - 1));
		std::string s;
		if (0 != (status & PCAN_ERROR_BUSOFF))
			s += " BUSOFF";
		if (0 != (status & PCAN_ERROR_BUSPASSIVE))
			s += " BUSPASSIVE";
		if (0 != (status & PCAN_ERROR_BUSHEAVY))
			s += " BUSHEAVY";
		if (0 != (status & PCAN_ERROR_BUSLIGHT))
			s += " BUSLIGHT";
		if (s.empty())
			s = (PCAN_ERROR_OK == status || PCAN_ERROR_QRCVEMPTY == status) ? " ok" : " status 0x" + std::to_string(status);
		return "bus" + s;
#else
		(void)channel;
		return "";
#endif
	}
}

int main(int argc, char *argv[])
{
	std::string driverName, interfaceName;
	int seconds = 10;
	bool probe = false;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		const bool hasValue = (i + 1 < argc);
		if (a == "--driver" && hasValue)
			driverName = argv[++i];
		else if (a == "--interface" && hasValue)
			interfaceName = argv[++i];
		else if (a == "--seconds" && hasValue)
			seconds = std::atoi(argv[++i]);
		else if (a == "--probe")
			probe = true;
		else
		{
			std::fprintf(stderr, "usage: can_sniff [--driver peak|socketcan|virtual] [--interface 1|can0] [--seconds 10] [--probe]\n");
			return 2;
		}
	}

	auto driver = CANDriverFactory::create(interfaceName, driverName);
	if (nullptr == driver)
	{
		std::fprintf(stderr, "can_sniff: no CAN driver for \"%s\" on \"%s\"\n", driverName.c_str(), interfaceName.c_str());
		return 1;
	}
	isobus::CANHardwareInterface::set_number_of_can_channels(1);
	isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, driver);
	isobus::CANHardwareInterface::get_can_frame_received_event_dispatcher().add_listener([](const isobus::CANMessageFrame &f) { rxCount++; print_frame("rx", f); });
	isobus::CANHardwareInterface::get_can_frame_transmitted_event_dispatcher().add_listener([](const isobus::CANMessageFrame &f) { txCount++; print_frame("tx", f); });
	started = std::chrono::steady_clock::now();
	if (!isobus::CANHardwareInterface::start() || !driver->get_is_valid())
	{
		std::fprintf(stderr, "can_sniff: the driver did not open\n");
		return 1;
	}
	const int channel = interfaceName.empty() ? 1 : std::atoi(interfaceName.c_str());
#ifdef ISOBUS_MACCANPCAN_AVAILABLE
	{
		// Let the driver deliver status frames, so a bus with no acknowledgement shows up
		std::uint32_t on = PCAN_PARAMETER_ON;
		const TPCANStatus r = CAN_SetValue(static_cast<TPCANHandle>(PCAN_USBBUS1 + channel - 1), PCAN_ALLOW_STATUS_FRAMES, &on, sizeof(on));
		std::printf("   0.000 -- status frames %s (0x%X)\n", PCAN_ERROR_OK == r ? "allowed" : "not supported by this driver", static_cast<unsigned>(r));
	}
#endif

	for (int s = 1; s <= seconds; s++)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (probe && 1 == s)
		{
			isobus::CANMessageFrame request;
			request.identifier = 0x18EAFFFEu; // request to global from the null address
			request.isExtendedFrame = true;
			request.dataLength = 3;
			request.data[0] = 0x00; // PGN EE00, address claim, little endian
			request.data[1] = 0xEE;
			request.data[2] = 0x00;
			request.channel = 0;
			int refused = 0;
			for (int n = 0; n < 5; n++)
			{
				if (!isobus::CANHardwareInterface::transmit_can_frame(request))
				{
					refused++;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			std::lock_guard<std::mutex> lock(printMutex);
			std::printf("%8.3f -- probe: 5 requests for address claim, %d refused by the driver (a full transmit queue means no acknowledgement on the wire)\n", 1.0, refused);
		}
		std::lock_guard<std::mutex> lock(printMutex);
		std::printf("%8.3f -- %u frames received, %u sent  %s\n", static_cast<double>(s), rxCount.load(), txCount.load(), bus_state(channel).c_str());
		std::fflush(stdout);
	}

	isobus::CANHardwareInterface::stop();
	std::lock_guard<std::mutex> lock(printMutex);
	std::printf("sources seen:");
	for (const auto &entry : rxBySource)
	{
		std::printf(" %02X(%u)", entry.first, entry.second);
	}
	std::printf("%s\n", rxBySource.empty() ? " none" : "");
	return 0;
}
