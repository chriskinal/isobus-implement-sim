// The task controller client: the stack's TaskControllerClient around the model, with the
// DDOP from the profile and the process data callbacks (docs/MODEL.md sections 2, 3, 6, 7).
#pragma once

#include "ddop.hpp"
#include "events.hpp"
#include "model.hpp"
#include "profile.hpp"

#include "isobus/hardware_integration/can_hardware_plugin.hpp"
#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/can_partnered_control_function.hpp"
#include "isobus/isobus/isobus_speed_distance_messages.hpp"
#include "isobus/isobus/isobus_task_controller_client.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sim
{
	class ImplementClient
	{
	public:
		ImplementClient(const Profile &profile, Model &model, std::mutex &modelMutex, Events &events);

		/// @brief Opens the bus on the driver, claims an address and starts the client. False if the bus cannot be opened.
		bool start(std::shared_ptr<isobus::CANHardwarePlugin> driver);

		/// @brief Call every tick, with the model mutex not held. Reads speed, reports changes as events.
		void update();

		void stop();

		const Ddop &ddop() const { return pool; }
		bool connected() const { return lastConnected; }
		int address() const { return lastAddress; }
		const char *state() const { return lastStateName; }
		std::shared_ptr<isobus::InternalControlFunction> control_function() const { return ecu; }
		std::uint32_t tc_address() const { return (tc && lastConnected) ? tc->get_address() : 0; }
		std::uint32_t tc_booms() const { return (client && lastConnected) ? client->get_connected_tc_number_booms_supported() : 0; }
		std::uint32_t tc_sections() const { return (client && lastConnected) ? client->get_connected_tc_number_sections_supported() : 0; }
		std::uint32_t tc_channels() const { return (client && lastConnected) ? client->get_connected_tc_number_channels_supported() : 0; }
		std::uint32_t tc_version() const { return (client && lastConnected) ? static_cast<std::uint32_t>(client->get_connected_tc_version()) : 0; }

	private:
		static bool request_value_callback(std::uint16_t element, std::uint16_t ddi, std::int32_t &value, void *parent);
		static bool value_command_callback(std::uint16_t element, std::uint16_t ddi, std::int32_t value, void *parent);
		static bool default_process_data_callback(std::uint16_t element, std::uint16_t ddi, isobus::TaskControllerClient::DefaultProcessDataSettings &settings, void *parent);

		bool request_value(std::uint16_t element, std::uint16_t ddi, std::int32_t &value);
		void value_command(std::uint16_t element, std::uint16_t ddi, std::int32_t value);
		void on_speed(int source, std::uint32_t mmPerSecond);
		void report_changes();

		const Profile &profile;
		Model &model;
		std::mutex &modelMutex;
		Events &events;
		Ddop pool;
		std::shared_ptr<isobus::InternalControlFunction> ecu;
		std::shared_ptr<isobus::PartneredControlFunction> tc;
		std::shared_ptr<isobus::TaskControllerClient> client;
		std::unique_ptr<isobus::SpeedMessagesInterface> speed;

		// Last reported, for the change events
		bool lastConnected = false;
		int lastState = -1;
		const char *lastStateName = "none";
		std::uint8_t lastAddress = 0xFF;
		std::vector<std::string> lastSetpoint, lastCommanded, lastActual;
		std::vector<bool> lastAuto, lastMaster;
		std::vector<double> lastTarget;
		double lastSpeedKmh = -1;
		bool lastSpeedKnown = false;
	};
}
