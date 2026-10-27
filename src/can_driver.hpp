// The CAN driver for --driver and --interface: the stack's own (socketcan, virtual, peak), or
// aowlink, an AOW link gateway at --interface host[:port] (aow_link_driver.hpp).
#pragma once

#include "isobus/hardware_integration/can_hardware_plugin.hpp"

#include <memory>
#include <string>

namespace sim
{
	/// @param clientName how this program names itself to an AOW link gateway
	std::shared_ptr<isobus::CANHardwarePlugin> create_can_driver(const std::string &interface, const std::string &driver, const std::string &clientName);
}
