// The I2C joystick module at 0x5A (one stick, buttons A to D and OK) for tools/aux_input, read
// through Linux's i2c-dev, on a Raspberry Pi's /dev/i2c-1 for one. The register map, from
// github.com/jitsengupta/PicoLibrary JoystickHandle.py, the only description of the module:
//
//   0x10, 0x11       stick X and Y, a byte each
//   0x20 to 0x24     buttons OK, C, A, B, D: the state of each as an event code, 8 when
//                    nothing is pressed (0 pressed, 1 released, 3 click, 5 long press, ...)
//
// A button counts as pressed while its code is not 8, as that library has it; the "joystick"
// event aux_input prints carries the raw codes, so what the module really does can be seen.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef __linux__
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

class I2CJoystick
{
public:
	static constexpr std::uint8_t DEFAULT_ADDRESS = 0x5A;
	static constexpr std::uint8_t NOTHING_PRESSED = 8;
	static constexpr const char *BUTTON_NAMES[5] = { "ok", "c", "a", "b", "d" }; // registers 0x20 to 0x24

	struct Reading
	{
		std::uint8_t x = 0, y = 0;
		std::array<std::uint8_t, 5> codes{}; // in register order, BUTTON_NAMES
		bool operator!=(const Reading &other) const { return x != other.x || y != other.y || codes != other.codes; }
		bool pressed(std::size_t button) const { return codes[button] != NOTHING_PRESSED; }
	};

	~I2CJoystick() { close(); }

	// Opens the bus and checks the module answers; false with the reason in error()
	bool open(const std::string &device, std::uint8_t address)
	{
#ifdef __linux__
		fd = ::open(device.c_str(), O_RDWR);
		if (fd < 0)
		{
			reason = "cannot open " + device;
			return false;
		}
		this->address = address;
		if (ioctl(fd, I2C_SLAVE, static_cast<unsigned long>(address)) < 0)
		{
			reason = "cannot address the module on " + device;
			close();
			return false;
		}
		Reading r;
		if (!read(r))
		{
			close();
			return false;
		}
		return true;
#else
		(void)device;
		(void)address;
		reason = "the I2C joystick needs Linux";
		return false;
#endif
	}

	void close()
	{
#ifdef __linux__
		if (fd >= 0)
		{
			::close(fd);
		}
#endif
		fd = -1;
	}

	// Both axes and the five buttons, one register at a time as the module is known to answer
	bool read(Reading &out)
	{
		std::uint8_t value = 0;
		if (!read_register(0x10, value))
			return false;
		out.x = value;
		if (!read_register(0x11, value))
			return false;
		out.y = value;
		for (std::uint8_t i = 0; i < out.codes.size(); i++)
		{
			if (!read_register(static_cast<std::uint8_t>(0x20 + i), value))
				return false;
			out.codes[i] = value;
		}
		return true;
	}

	const std::string &error() const { return reason; }

private:
	// An SMBus read byte data: the register number written, then a byte read after a repeated
	// start, which is what the module answers to and what i2c-stub (the tests) implements
	bool read_register(std::uint8_t reg, std::uint8_t &value)
	{
#ifdef __linux__
		i2c_smbus_data data{};
		i2c_smbus_ioctl_data transfer{ I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &data };
		if (ioctl(fd, I2C_SMBUS, &transfer) >= 0)
		{
			value = data.byte;
			return true;
		}
		char text[64];
		std::snprintf(text, sizeof(text), "no answer from 0x%02X reading register 0x%02X", address, reg);
		reason = text;
#else
		(void)reg;
		(void)value;
#endif
		return false;
	}

	int fd = -1;
	std::uint16_t address = DEFAULT_ADDRESS;
	std::string reason;
};

// An 8-bit axis to an auxiliary input's 0 to 64255, the centre (taken at start) at half, with a
// deadband round it and each side stretched to the furthest the stick has gone
struct JoystickAxis
{
	static constexpr std::uint16_t MAX = 64255;
	static constexpr int DEADBAND = 4;
	int centre = 128, low = 0, high = 255;

	void set_centre(int value)
	{
		centre = value;
		low = std::max(0, centre - 100);
		high = std::min(255, centre + 100);
	}

	std::uint16_t scale(int raw)
	{
		low = std::min(low, raw);
		high = std::max(high, raw);
		const double half = MAX / 2.0;
		if (std::abs(raw - centre) <= DEADBAND)
		{
			return static_cast<std::uint16_t>(half);
		}
		const double v = raw > centre ? half + half * (raw - centre - DEADBAND) / std::max(1, high - centre - DEADBAND) : half - half * (centre - DEADBAND - raw) / std::max(1, centre - DEADBAND - low);
		return static_cast<std::uint16_t>(std::min<double>(MAX, std::max(0.0, v)));
	}
};
