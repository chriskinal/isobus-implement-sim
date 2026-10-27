#include "aow_link_driver.hpp"

#include "isobus/isobus/can_stack_logger.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sim
{
	namespace
	{
		enum Type : std::uint8_t
		{
			Hello = 1,
			FrameMsg = 2,
			Heartbeat = 3,
			Status = 4,
			Close = 5
		};
		constexpr std::uint8_t VERSION = 1;
		constexpr std::uint64_t HEARTBEAT_MS = 100;
		constexpr std::uint64_t SILENCE_MS = 1000;
		constexpr std::uint64_t RETRY_MS = 250;
		constexpr int CONNECT_MS = 500;
		constexpr std::size_t MAX_PENDING = 64 * 1024; // the gateway is not reading: the link is dead

		std::uint64_t now_ms()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		void le16(std::string &out, std::uint16_t v)
		{
			out += static_cast<char>(v & 0xFF);
			out += static_cast<char>(v >> 8);
		}

		void le32(std::string &out, std::uint32_t v)
		{
			for (int i = 0; i < 4; i++)
			{
				out += static_cast<char>((v >> (8 * i)) & 0xFF);
			}
		}

		std::uint32_t rd32(const std::string &b, std::size_t at)
		{
			std::uint32_t v = 0;
			for (int i = 3; i >= 0; i--)
			{
				v = (v << 8) | static_cast<std::uint8_t>(b[at + i]);
			}
			return v;
		}

		void message(std::string &out, std::uint8_t type, const std::string &body)
		{
			out += static_cast<char>(type);
			out += static_cast<char>(body.size() > 255 ? 255 : body.size());
			out.append(body, 0, 255);
		}

		// A send that never raises SIGPIPE
		ssize_t send_quietly(int fd, const char *data, std::size_t size)
		{
#ifdef MSG_NOSIGNAL
			return ::send(fd, data, size, MSG_NOSIGNAL);
#else
			return ::send(fd, data, size, 0); // SO_NOSIGPIPE is set on the socket
#endif
		}

		const char *close_reason(std::uint8_t reason)
		{
			switch (reason)
			{
				case 1:
					return "version";
				case 2:
					return "busy";
				case 3:
					return "timeout";
				case 4:
					return "protocol error";
				case 5:
					return "shutting down";
				default:
					return "closed";
			}
		}
	}

	AowLinkDriver::AowLinkDriver(std::string address, std::string clientName, std::uint8_t channel) :
	  name(std::move(clientName)),
	  channel(channel)
	{
		const std::size_t colon = address.rfind(':');
		if (colon != std::string::npos && address.find(':') == colon)
		{
			host = address.substr(0, colon);
			port = static_cast<std::uint16_t>(std::atoi(address.c_str() + colon + 1));
		}
		else
		{
			host = address;
		}
	}

	AowLinkDriver::~AowLinkDriver()
	{
		close();
	}

	void AowLinkDriver::open()
	{
		if (valid)
		{
			return;
		}
		if (host.empty() || port == 0)
		{
			set_error("--interface must be the gateway's host[:port]");
			return;
		}
		// The first connection is made here, so a gateway that is not there fails the start
		if (!connect_link(3000))
		{
			return;
		}
		valid = true;
		running = true;
		worker = std::thread([this] { run(); });
	}

	void AowLinkDriver::close()
	{
		running = false;
		if (worker.joinable())
		{
			worker.join();
		}
		if (fd >= 0)
		{
			std::string bye;
			std::string body(1, static_cast<char>(5)); // shutting down
			body += static_cast<char>(0);
			message(bye, Close, body);
			send_quietly(fd, bye.data(), bye.size());
			close_link("closed");
		}
		valid = false;
		receivedCondition.notify_all();
	}

	std::string AowLinkDriver::get_last_error() const
	{
		std::lock_guard<std::mutex> lock(errorMutex);
		return lastError;
	}

	void AowLinkDriver::set_error(const std::string &text)
	{
		{
			std::lock_guard<std::mutex> lock(errorMutex);
			if (lastError == text)
			{
				return;
			}
			lastError = text;
		}
		LOG_WARNING("[AOW link] " + text);
	}

	bool AowLinkDriver::read_frame(isobus::CANMessageFrame &frame)
	{
		std::unique_lock<std::mutex> lock(receivedMutex);
		if (!receivedCondition.wait_for(lock, std::chrono::milliseconds(100), [this] { return !received.empty() || !valid; }) || received.empty())
		{
			return false;
		}
		frame = received.front();
		received.pop_front();
		return true;
	}

	bool AowLinkDriver::write_frame(const isobus::CANMessageFrame &frame)
	{
		std::string out;
		std::string body;
		body += static_cast<char>(channel);
		body += static_cast<char>(frame.isExtendedFrame ? 1 : 0);
		le32(body, frame.identifier);
		le32(body, 0);
		body.append(reinterpret_cast<const char *>(frame.data), frame.dataLength > 8 ? 8 : frame.dataLength);
		message(out, FrameMsg, body);
		std::lock_guard<std::mutex> lock(sendMutex);
		if (!linked())
		{
			droppedFrames++;
			return true; // the link is down: dropped, never held for the next one
		}
		pending += out;
		if (!flush_locked())
		{
			droppedFrames++;
		}
		return true;
	}

	bool AowLinkDriver::flush_locked()
	{
		while (!pending.empty() && fd >= 0)
		{
			const ssize_t sent = send_quietly(fd, pending.data(), pending.size());
			if (sent > 0)
			{
				pending.erase(0, static_cast<std::size_t>(sent));
			}
			else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			{
				break;
			}
			else
			{
				up = false; // the worker closes it
				return false;
			}
		}
		if (pending.size() > MAX_PENDING)
		{
			up = false;
			return false;
		}
		return true;
	}

	// TCP connect within connectMs, HELLO both ways; the socket left non-blocking
	bool AowLinkDriver::connect_link(int connectMs)
	{
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *found = nullptr;
		if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found)
		{
			set_error("cannot find " + host);
			return false;
		}
		int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s < 0)
		{
			freeaddrinfo(found);
			set_error("no socket");
			return false;
		}
		fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
		const int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
		setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
		int r = ::connect(s, found->ai_addr, found->ai_addrlen);
		freeaddrinfo(found);
		pollfd p{ s, POLLOUT, 0 };
		int error = 0;
		socklen_t length = sizeof error;
		if (r < 0 && (errno != EINPROGRESS || poll(&p, 1, connectMs) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0))
		{
			::close(s);
			set_error("no gateway at " + host + ":" + std::to_string(port));
			return false;
		}

		std::string hello("AOWL", 4);
		hello += static_cast<char>(VERSION);
		hello += static_cast<char>(0); // control
		hello += static_cast<char>(name.size() > 200 ? 200 : name.size());
		hello.append(name, 0, 200);
		std::string out;
		message(out, Hello, hello);
		send_quietly(s, out.data(), out.size());

		// Its HELLO, or CLOSE, within a second
		std::string got;
		const std::uint64_t until = now_ms() + 1000;
		while (now_ms() < until)
		{
			pollfd q{ s, POLLIN, 0 };
			if (poll(&q, 1, 50) != 1)
			{
				continue;
			}
			char buffer[1024];
			const ssize_t n = ::recv(s, buffer, sizeof buffer, 0);
			if (n <= 0)
			{
				break;
			}
			got.append(buffer, static_cast<std::size_t>(n));
			if (got.size() >= 2 && got.size() >= 2u + static_cast<std::uint8_t>(got[1]))
			{
				const std::uint8_t type = static_cast<std::uint8_t>(got[0]);
				const std::string body = got.substr(2, static_cast<std::uint8_t>(got[1]));
				if (type == Hello && body.size() >= 7 && body.compare(0, 4, "AOWL") == 0 && static_cast<std::uint8_t>(body[4]) == VERSION)
				{
					const std::size_t nameLength = static_cast<std::uint8_t>(body[6]);
					const std::string gateway = body.substr(7, nameLength);
					in = got.substr(2 + body.size()); // STATUS and the rest, for the worker
					{
						std::lock_guard<std::mutex> lock(sendMutex);
						pending.clear();
						fd = s;
						up = true;
					}
					heardAtMs = now_ms();
					nextHeartbeatMs = 0;
					heardTime = 0;
					{
						std::lock_guard<std::mutex> lock(errorMutex);
						lastError.clear();
					}
					LOG_INFO("[AOW link] connected to \"" + gateway + "\" at " + host + ":" + std::to_string(port));
					return true;
				}
				if (type == Close && !body.empty())
				{
					const std::size_t textLength = body.size() > 1 ? static_cast<std::uint8_t>(body[1]) : 0;
					set_error(std::string("the gateway said ") + close_reason(static_cast<std::uint8_t>(body[0])) + (textLength ? ": " + body.substr(2, textLength) : ""));
				}
				else
				{
					set_error("no AOW link version 1 at " + host + ":" + std::to_string(port));
				}
				break;
			}
		}
		if (got.empty())
		{
			set_error("no HELLO from " + host + ":" + std::to_string(port));
		}
		::close(s);
		return false;
	}

	void AowLinkDriver::close_link(const std::string &why)
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (fd < 0)
		{
			return;
		}
		linger l{ 1, 0 }; // nothing unsent is kept
		setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
		::close(fd);
		fd = -1;
		up = false;
		pending.clear();
		in.clear();
		if (why != "closed")
		{
			LOG_WARNING("[AOW link] link down: " + why);
		}
	}

	void AowLinkDriver::heartbeat()
	{
		const std::uint64_t now = now_ms();
		std::string body;
		le32(body, static_cast<std::uint32_t>(now) ? static_cast<std::uint32_t>(now) : 1);
		le32(body, heardTime);
		le16(body, static_cast<std::uint16_t>(heardTime ? (now - heardTimeAtMs > 65535 ? 65535 : now - heardTimeAtMs) : 0));
		std::lock_guard<std::mutex> lock(sendMutex);
		message(pending, Heartbeat, body);
		flush_locked();
	}

	void AowLinkDriver::handle(std::uint8_t type, const std::string &body)
	{
		if (type == FrameMsg && body.size() >= 10 && static_cast<std::uint8_t>(body[0]) == channel)
		{
			isobus::CANMessageFrame frame{};
			frame.channel = 0;
			frame.isExtendedFrame = (body[1] & 1) != 0;
			frame.identifier = rd32(body, 2);
			frame.dataLength = static_cast<std::uint8_t>(body.size() - 10 > 8 ? 8 : body.size() - 10);
			std::memcpy(frame.data, body.data() + 10, frame.dataLength);
			frame.timestamp_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			{
				std::lock_guard<std::mutex> lock(receivedMutex);
				received.push_back(frame);
			}
			receivedCondition.notify_one();
		}
		else if (type == Heartbeat && body.size() >= 10)
		{
			heardTime = rd32(body, 0);
			heardTimeAtMs = now_ms();
		}
		else if (type == Status && body.size() >= 12 && static_cast<std::uint8_t>(body[0]) == channel)
		{
			static const char *states[] = { "error active", "error warning", "error passive", "bus-off", "down" };
			const std::uint8_t state = static_cast<std::uint8_t>(body[1]);
			if (state >= 2)
			{
				set_error(std::string("the gateway's bus is ") + states[state > 4 ? 4 : state]);
			}
		}
		else if (type == Close)
		{
			const std::size_t textLength = body.size() > 1 ? static_cast<std::uint8_t>(body[1]) : 0;
			up = false;
			set_error(std::string("the gateway closed the link: ") + close_reason(body.empty() ? 0 : static_cast<std::uint8_t>(body[0])) + (textLength ? ", " + body.substr(2, textLength) : ""));
		}
		// anything newer: skipped by its length
	}

	void AowLinkDriver::run()
	{
		std::uint64_t nextTry = 0;
		while (running)
		{
			const std::uint64_t now = now_ms();
			if (fd < 0 || !up)
			{
				if (fd >= 0)
				{
					close_link("the gateway closed it or stopped reading");
				}
				if (now < nextTry)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				if (!connect_link(CONNECT_MS))
				{
					nextTry = now_ms() + RETRY_MS;
					continue;
				}
			}
			if (now >= nextHeartbeatMs)
			{
				nextHeartbeatMs = now + HEARTBEAT_MS;
				heartbeat();
			}
			// Whatever was already read with the HELLO, then the socket
			std::size_t at = 0;
			while (in.size() - at >= 2 && in.size() - at >= 2u + static_cast<std::uint8_t>(in[at + 1]))
			{
				const std::size_t n = static_cast<std::uint8_t>(in[at + 1]);
				handle(static_cast<std::uint8_t>(in[at]), in.substr(at + 2, n));
				at += 2 + n;
			}
			in.erase(0, at);

			pollfd p{ fd, POLLIN, 0 };
			{
				std::lock_guard<std::mutex> lock(sendMutex);
				if (!pending.empty())
				{
					p.events |= POLLOUT;
				}
			}
			if (poll(&p, 1, 10) > 0)
			{
				if (p.revents & POLLOUT)
				{
					std::lock_guard<std::mutex> lock(sendMutex);
					flush_locked();
				}
				if (p.revents & (POLLIN | POLLHUP | POLLERR))
				{
					char buffer[4096];
					const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
					if (n > 0)
					{
						in.append(buffer, static_cast<std::size_t>(n));
						heardAtMs = now_ms();
					}
					else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
					{
						close_link(n == 0 ? "closed by the gateway" : std::string("error ") + std::strerror(errno));
						nextTry = 0;
						continue;
					}
				}
			}
			if (now_ms() - heardAtMs > SILENCE_MS)
			{
				close_link("nothing heard for 1 s");
				nextTry = 0;
			}
		}
	}
}
