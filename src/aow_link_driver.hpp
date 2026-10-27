// A CAN driver that reaches the bus through an AOW link gateway over TCP (--driver aowlink): the
// ESP32 gateway or aow-linkd. The protocol is SharpIsoTerm's docs/TRANSPORT.md section 5, version
// 1; this is its client end, as a control client on one of the gateway's channels.
//
// The link is kept up by a thread of its own: a HEARTBEAT every 100 ms, the link closed after
// 1 s with nothing heard, a new connection at once and then every 250 ms. While the link is down
// the stack keeps running and what it sends is dropped (counted), as the protocol asks of a
// client; a frame is never held for a later connection.
#pragma once

#include "isobus/hardware_integration/can_hardware_plugin.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace sim
{
	class AowLinkDriver : public isobus::CANHardwarePlugin
	{
	public:
		/// @param address "host[:port]" (port 2323 by default), a name such as aow-gateway-d430.local
		/// @param clientName what the gateway logs and tells a second terminal it is busy with
		AowLinkDriver(std::string address, std::string clientName, std::uint8_t channel = 0);
		~AowLinkDriver() override;

		std::string get_name() const override { return "AOW link"; }
		bool get_is_valid() const override { return valid; }
		void close() override;
		void open() override;
		bool read_frame(isobus::CANMessageFrame &frame) override;
		bool write_frame(const isobus::CANMessageFrame &frame) override;
		std::string get_last_error() const override;

		bool linked() const { return fd >= 0 && up; }
		std::uint64_t dropped() const { return droppedFrames; }

	private:
		bool connect_link(int connectMs);
		void close_link(const std::string &why);
		void run();
		bool flush_locked(); ///< sends what it can of pending; false if the socket failed
		void handle(std::uint8_t type, const std::string &body);
		void heartbeat();
		void set_error(const std::string &text);

		std::string host;
		std::uint16_t port = 2323;
		std::string name;
		std::uint8_t channel;

		std::atomic<bool> valid{ false };
		std::atomic<bool> running{ false };
		std::atomic<bool> up{ false };
		std::atomic<int> fd{ -1 };
		std::thread worker;

		std::mutex sendMutex; ///< the socket's write side and pending
		std::string pending; ///< bytes not yet taken by the socket
		std::string in; ///< bytes received, not yet a whole message

		std::mutex receivedMutex;
		std::condition_variable receivedCondition;
		std::deque<isobus::CANMessageFrame> received;

		std::uint64_t heardAtMs = 0, nextHeartbeatMs = 0;
		std::uint32_t heardTime = 0; ///< the gateway's last HEARTBEAT time, echoed back
		std::uint64_t heardTimeAtMs = 0;
		std::atomic<std::uint64_t> droppedFrames{ 0 };

		mutable std::mutex errorMutex;
		std::string lastError;
	};
}
