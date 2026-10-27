// The I2C joystick module at 0x5A (one stick, buttons A to D and OK) for tools/aux_input, read
// through Linux's i2c-dev, on a Raspberry Pi's /dev/i2c-1 for one. The register map, from the
// vendor's Arduino demo (JoystickHandle.h, which agrees with github.com/jitsengupta/PicoLibrary
// JoystickHandle.py):
//
//   0x10, 0x11       stick X and Y, a byte each (0x12, 0x13: a second stick, not on this board)
//   0x20 to 0x24     buttons OK, C, A, B, D: the last event of each as a code, 0 press down,
//                    1 press up, 2 press repeat, 3 single click, 4 double click, 5 long press
//                    start, 6 long press hold, 8 nothing
//
// The codes are events, not levels: after a release the module says 1 (up), then 3 or 4 (the
// click) until its click timer runs out, and only then 8. So a button is held while its code is
// 0, 2, 5 or 6; counting anything but 8 as held (the demo's ButtonPressed) would let go late.
// The "joystick" event aux_input prints carries the raw codes, so what the module really does
// can be seen.
//
// Where no I2C bus is free (the AOW HAT has GPIO2 and GPIO3 as outputs), open_serial() reads the
// same registers through an ESP32 on a USB serial port: tools/i2c_bridge, or SharpIsoTerm's ESP32
// AOW link gateway, which answers the same commands and writes its log as lines starting '#'.
// open_tcp() reaches the gateway's bridge over the network instead (its port 2324). Through a
// bridge the seven registers are asked for in one write and the answers read after, so a reading
// costs one round trip, not seven.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef __linux__
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#endif
#ifndef _WIN32
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

class I2CJoystick
{
public:
	static constexpr std::uint8_t DEFAULT_ADDRESS = 0x5A;
	static constexpr std::uint8_t PRESS_DOWN = 0, PRESS_REPEAT = 2, LONG_PRESS_START = 5, LONG_PRESS_HOLD = 6, NOTHING_PRESSED = 8;
	static constexpr const char *BUTTON_NAMES[5] = { "ok", "c", "a", "b", "d" }; // registers 0x20 to 0x24

	struct Reading
	{
		std::uint8_t x = 0, y = 0;
		std::array<std::uint8_t, 5> codes{}; // in register order, BUTTON_NAMES
		bool operator!=(const Reading &other) const { return x != other.x || y != other.y || codes != other.codes; }
		bool pressed(std::size_t button) const
		{
			const std::uint8_t c = codes[button];
			return c == PRESS_DOWN || c == PRESS_REPEAT || c == LONG_PRESS_START || c == LONG_PRESS_HOLD;
		}
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

	// The same through tools/i2c_bridge on a serial port: waits for the bridge (opening the port
	// usually resets the board) and checks the module answers
	bool open_serial(const std::string &device, std::uint8_t address)
	{
#ifndef _WIN32
		fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0)
		{
			reason = "cannot open " + device;
			return false;
		}
		termios options{};
		tcgetattr(fd, &options);
		cfmakeraw(&options);
		cfsetispeed(&options, B115200);
		cfsetospeed(&options, B115200);
		options.c_cflag |= CLOCAL | CREAD;
		tcsetattr(fd, TCSANOW, &options);
		bridge = true;
		tty = true;
		this->address = address;
		return handshake(device);
#else
		(void)device;
		(void)address;
		reason = "the serial bridge needs a POSIX system";
		return false;
#endif
	}

	// The gateway's bridge over TCP, "host[:port]" (2324 by default)
	bool open_tcp(const std::string &where, std::uint8_t address)
	{
#ifndef _WIN32
		std::string host = where, port = "2324";
		const std::size_t colon = where.rfind(':');
		if (colon != std::string::npos)
		{
			host = where.substr(0, colon);
			port = where.substr(colon + 1);
		}
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *found = nullptr;
		if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found)
		{
			reason = "cannot find " + host;
			return false;
		}
		fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		const int one = 1;
		bool connected = false;
		if (fd >= 0)
		{
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
			setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
			const int r = ::connect(fd, found->ai_addr, found->ai_addrlen);
			pollfd p{ fd, POLLOUT, 0 };
			int error = 0;
			socklen_t length = sizeof error;
			connected = r == 0 || (errno == EINPROGRESS && ::poll(&p, 1, 3000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0);
		}
		freeaddrinfo(found);
		if (!connected)
		{
			reason = "no bridge at " + host + ":" + port;
			close();
			return false;
		}
		bridge = true;
		tty = false;
		this->address = address;
		return handshake(where);
#else
		(void)where;
		(void)address;
		reason = "the bridge needs a POSIX system";
		return false;
#endif
	}

	void close()
	{
#ifndef _WIN32
		if (fd >= 0)
		{
			::close(fd);
		}
#endif
		fd = -1;
		bridge = false;
		pending.clear();
	}

	// Both axes and the five buttons, one register at a time as the module is known to answer
	bool read(Reading &out)
	{
		if (bridge)
		{
			return read_bridge(out);
		}
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

	// "?" until the bridge says "i2c-bridge 1" (a board reset by the port's opening takes a
	// second or two), then what else is waiting dropped, and one reading to check the module
	bool handshake(const std::string &where)
	{
		std::string reply;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (send_line("?") && read_line(reply, 500) && reply == "i2c-bridge 1")
			{
				while (read_line(reply, 100)) // answers to earlier "?"s, the start banner, log lines
				{
				}
				Reading r;
				if (!read(r))
				{
					close();
					return false;
				}
				return true;
			}
		}
		reason = "no i2c-bridge answering on " + where;
		close();
		return false;
	}

	// The seven registers asked for in one write, "r <addr> <reg>" each, then their answers in
	// order: two hex digits or "e". A late or garbled answer drops whatever is waiting, so the
	// next reading starts clean.
	bool read_bridge(Reading &out)
	{
		static constexpr std::uint8_t REGISTERS[7] = { 0x10, 0x11, 0x20, 0x21, 0x22, 0x23, 0x24 };
		std::string ask;
		char text[128];
		for (std::uint8_t reg : REGISTERS)
		{
			std::snprintf(text, sizeof(text), "%sr %02x %02x", ask.empty() ? "" : "\n", address, reg);
			ask += text;
		}
		if (!send_line(ask))
		{
			reason = "the bridge went away";
			return false;
		}
		std::uint8_t values[7] = {};
		std::string reply;
		for (std::size_t i = 0; i < 7; i++)
		{
			bool answered = read_line(reply, 200);
			while (answered && (reply == "i2c-bridge 1" || (!reply.empty() && reply[0] == '#'))) // a reset's banner, log lines
			{
				answered = read_line(reply, 200);
			}
			char *end = nullptr;
			const unsigned long v = answered ? std::strtoul(reply.c_str(), &end, 16) : 256;
			if (!answered || reply.size() != 2 || !end || *end != '\0' || v > 0xFF)
			{
				discard();
				std::snprintf(text, sizeof(text), "no answer from 0x%02X reading register 0x%02X through the bridge%s", address, REGISTERS[i], reply == "e" ? " (the bridge got no ACK)" : "");
				reason = text;
				return false;
			}
			values[i] = static_cast<std::uint8_t>(v);
		}
		out.x = values[0];
		out.y = values[1];
		for (std::size_t b = 0; b < out.codes.size(); b++)
		{
			out.codes[b] = values[2 + b];
		}
		return true;
	}

	void discard()
	{
#ifndef _WIN32
		if (tty)
		{
			tcflush(fd, TCIFLUSH);
		}
		else
		{
			std::string ignored;
			while (read_line(ignored, 50))
			{
			}
		}
#endif
		pending.clear();
	}

	bool send_line(const std::string &text)
	{
#ifndef _WIN32
		const std::string out = text + "\n";
#ifdef MSG_NOSIGNAL
		if (!tty) // a closed socket must not raise SIGPIPE
		{
			return ::send(fd, out.data(), out.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(out.size());
		}
#endif
		return ::write(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size());
#else
		(void)text;
		return false;
#endif
	}

	// One line from the bridge, without its end, within timeoutMs
	bool read_line(std::string &out, int timeoutMs)
	{
#ifndef _WIN32
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (true)
		{
			const std::size_t end = pending.find('\n');
			if (end != std::string::npos)
			{
				out = pending.substr(0, end);
				pending.erase(0, end + 1);
				if (!out.empty() && out.back() == '\r')
				{
					out.pop_back();
				}
				return true;
			}
			const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
			pollfd p{ fd, POLLIN, 0 };
			if (left <= 0 || ::poll(&p, 1, left) <= 0)
			{
				return false;
			}
			char buffer[64];
			const ssize_t n = ::read(fd, buffer, sizeof(buffer));
			if (n <= 0)
			{
				return false;
			}
			pending.append(buffer, static_cast<std::size_t>(n));
		}
#else
		(void)out;
		(void)timeoutMs;
		return false;
#endif
	}

	int fd = -1;
	bool bridge = false; // through tools/i2c_bridge or the gateway, a line each way
	bool tty = false; // that bridge on a serial port, not TCP
	std::string pending; // bytes from the bridge not yet taken as a line
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
