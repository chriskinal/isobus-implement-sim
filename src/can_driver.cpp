#include "can_driver.hpp"

#include "aow_link_driver.hpp"
#include "create_can_driver.hpp"

#include <cstdio>
#include <unistd.h>

namespace sim
{
	std::shared_ptr<isobus::CANHardwarePlugin> create_can_driver(const std::string &interface, const std::string &driver, const std::string &clientName)
	{
		if (driver == "aowlink")
		{
			if (interface.empty())
			{
				std::fprintf(stderr, "--driver aowlink needs --interface host[:port], the gateway\n");
				return nullptr;
			}
			char host[64] = {};
			gethostname(host, sizeof host - 1);
			return std::make_shared<AowLinkDriver>(interface, clientName + " on " + host);
		}
		return CANDriverFactory::create(interface, driver);
	}
}
