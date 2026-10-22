// The virtual terminal client: the profile's pool on a terminal, the stack's seeder screen
// with behaviour, any other pool shown as it is (docs/PLAN.md phase 3).
#pragma once

#include "events.hpp"
#include "model.hpp"
#include "profile.hpp"

#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/can_partnered_control_function.hpp"
#include "isobus/isobus/isobus_virtual_terminal_client.hpp"
#include "isobus/isobus/isobus_virtual_terminal_client_update_helper.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sim
{
	class ImplementClient;

	class TerminalClient
	{
	public:
		TerminalClient(const Profile &profile, Model &model, std::mutex &modelMutex, Events &events, ImplementClient &tc);

		/// @brief Loads the profile's pool and starts the client on the implement's control function.
		/// False with no pool: the implement runs on without a terminal, and the event says why.
		bool start(std::shared_ptr<isobus::InternalControlFunction> ecu, const std::string &profileDirectory);
		/// @brief Call every tick: connection changes, the screen, the alarms.
		void update();
		void stop();

		bool active() const { return client != nullptr; }
		bool connected() const { return lastConnected; }
		std::uint16_t active_mask() const { return currentMask; }
		std::string pool_path() const { return poolPath; }

		/// @brief Leaves the terminal: the client stops before the control function goes (fault restart, first half).
		void leave();
		/// @brief Rejoins on a new control function (fault restart, second half).
		void rejoin(std::shared_ptr<isobus::InternalControlFunction> ecu);
		/// @brief Uploads the pool again under another version label, padded by extra bytes if asked (fault version, fault bigpool).
		bool reupload(const std::string &label, std::size_t padBytes);

		// Commands (docs/PLAN.md section 6)
		bool raise_alarm(const std::string &name, std::string &error);
		void clear_alarms();
		void press_key(std::uint16_t objectId);
		bool change_mask(std::uint16_t maskId);
		bool set_value(std::uint16_t objectId, std::uint32_t value);
		std::vector<std::string> alarm_names() const;

	private:
		void connect();
		struct Alarm
		{
			std::string name;
			std::uint16_t mask;
			double afterSeconds;
			bool condition = false; ///< The cause is present now
			std::uint32_t sinceMs = 0; ///< When the cause appeared
			bool active = false; ///< Raised and not yet acknowledged or cleared
			bool acknowledged = false;
			bool forced = false; ///< By the alarm command
		};

		void on_key(const isobus::VirtualTerminalClient::VTKeyEvent &event, const char *source);
		void on_value(const isobus::VirtualTerminalClient::VTChangeNumericValueEvent &event);
		void on_mask(const isobus::VirtualTerminalClient::VTChangeActiveMaskEvent &event);
		void act_on_key(std::uint16_t objectId);
		void show_mask(std::uint16_t maskId);
		void update_screen();
		void update_alarms();
		void acknowledge_first_alarm();

		const Profile &profile;
		Model &model;
		std::mutex &modelMutex;
		Events &events;
		ImplementClient &tc;
		std::shared_ptr<isobus::PartneredControlFunction> vt;
		std::shared_ptr<isobus::VirtualTerminalClient> client;
		std::unique_ptr<isobus::VirtualTerminalClientUpdateHelper> helper;
		std::shared_ptr<isobus::InternalControlFunction> ecu;
		std::vector<std::uint8_t> pool; ///< The file's bytes
		std::vector<std::uint8_t> uploaded; ///< What goes to the terminal: the pool, padded if asked
		std::string poolPath;
		std::string versionLabel;
		bool behaviour = false; ///< The pool is pools/base.iop, whose objects we know
		std::mutex vtMutex; ///< Around the helper and every send
		std::mutex sessionMutex; ///< update() against leave, rejoin and reupload, which run on the command thread
		std::vector<Alarm> alarms;
		bool alarmsEnabled = true;
		bool lastConnected = false;
		std::uint16_t currentMask = 0xFFFF;
		std::uint16_t wantedMask = 0xFFFF; ///< The data mask to return to after an alarm
		std::uint32_t lastScreenMs = 0;
	};
}
