#include "vt_client.hpp"
#include "tc_client.hpp"

#include "../pools/aux_functions_ids.hpp"
#include "../pools/base_pool_ids.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/utility/iop_file_interface.hpp"

#include <cmath>
#include <fstream>

namespace sim
{
	using VTC = isobus::VirtualTerminalClient;

	namespace
	{
		const char *code_name(VTC::KeyActivationCode code)
		{
			switch (code)
			{
				case VTC::KeyActivationCode::ButtonUnlatchedOrReleased:
					return "released";
				case VTC::KeyActivationCode::ButtonPressedOrLatched:
					return "pressed";
				case VTC::KeyActivationCode::ButtonStillHeld:
					return "held";
				case VTC::KeyActivationCode::ButtonPressAborted:
					return "aborted";
			}
			return "?";
		}

		bool file_exists(const std::string &path)
		{
			std::ifstream in(path);
			return in.good();
		}
	}

	TerminalClient::TerminalClient(const Profile &p, Model &m, std::mutex &mx, Events &ev, ImplementClient &t) :
	  profile(p),
	  model(m),
	  modelMutex(mx),
	  events(ev),
	  tc(t)
	{
		alarms.push_back({ "noSpeed", noSpeed_AlarmMask, p.alarmNoSpeed });
		alarms.push_back({ "noTaskController", noTaskController_AlarmMask, p.alarmNoTaskController });
	}

	bool TerminalClient::start(std::shared_ptr<isobus::InternalControlFunction> ecuToUse, const std::string &profileDirectory)
	{
		ecu = ecuToUse;
		if (profile.pool.empty())
		{
			events.emit("vt", { { "connected", false }, { "pool", false }, { "error", "the profile names no pool" } });
			return false;
		}
		// The path as given, then beside the profiles directory, then beside the profile
		for (const std::string &candidate : { profile.pool, profileDirectory + "/../" + profile.pool, profileDirectory + "/" + profile.pool })
		{
			if (file_exists(candidate))
			{
				poolPath = candidate;
				break;
			}
		}
		if (poolPath.empty())
		{
			events.emit("vt", { { "connected", false }, { "pool", false }, { "error", "pool file not found: " + profile.pool } });
			return false;
		}
		pool = isobus::IOPFileInterface::read_iop_file(poolPath);
		if (pool.empty())
		{
			events.emit("vt", { { "connected", false }, { "pool", false }, { "error", "pool file empty or unreadable: " + poolPath } });
			return false;
		}
		versionLabel = isobus::IOPFileInterface::hash_object_pool_to_version(pool);
		const auto slash = poolPath.find_last_of('/');
		const std::string file = poolPath.substr(slash == std::string::npos ? 0 : slash + 1);
		behaviour = file == "base.iop";
		auxPool = file == "aux_functions.iop";

		uploaded = pool;
		connect();
		wantedMask = behaviour ? mainRunscreen_DataMask : 0xFFFF;
		events.emit("vt", { { "connected", false }, { "pool", true }, { "path", poolPath }, { "bytes", pool.size() }, { "version", versionLabel }, { "behaviour", behaviour }, { "auxFunctions", auxPool } });
		return true;
	}

	void TerminalClient::connect()
	{
		const std::vector<isobus::NAMEFilter> vtFilters = {
			isobus::NAMEFilter(isobus::NAME::NAMEParameters::FunctionCode, static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal))
		};
		vt = isobus::CANNetworkManager::CANNetwork.create_partnered_control_function(0, vtFilters);
		client = std::make_shared<VTC>(vt, ecu);
		client->set_object_pool(0, uploaded.data(), static_cast<std::uint32_t>(uploaded.size()), versionLabel);
		client->get_vt_soft_key_event_dispatcher().add_listener([this](const VTC::VTKeyEvent &e) { on_key(e, "softkey"); });
		client->get_vt_button_event_dispatcher().add_listener([this](const VTC::VTKeyEvent &e) { on_key(e, "button"); });
		client->get_vt_change_numeric_value_event_dispatcher().add_listener([this](const VTC::VTChangeNumericValueEvent &e) { on_value(e); });
		client->get_vt_change_active_mask_event_dispatcher().add_listener([this](const VTC::VTChangeActiveMaskEvent &e) { on_mask(e); });
		client->get_auxiliary_function_event_dispatcher().add_listener([this](const VTC::AuxiliaryFunctionEvent &e) { on_aux(e); });
		client->initialize(true);

		helper = std::make_unique<isobus::VirtualTerminalClientUpdateHelper>(client);
		if (behaviour)
		{
			const std::uint16_t pointers[] = { section1EnableState_ObjPtr, section2EnableState_ObjPtr, section3EnableState_ObjPtr, section4EnableState_ObjPtr, section5EnableState_ObjPtr, section6EnableState_ObjPtr };
			const std::uint16_t rects[] = { section1Status_OutRect, section2Status_OutRect, section3Status_OutRect, section4Status_OutRect, section5Status_OutRect, section6Status_OutRect };
			for (int i = 0; i < 6; i++)
			{
				helper->add_tracked_numeric_value(pointers[i], offButtonSliderSmall_OutPict);
				helper->add_tracked_attribute(rects[i], 5, static_cast<std::uint32_t>(solidRed_FillAttr));
			}
			helper->add_tracked_numeric_value(autoManual_ObjPtr, autoMode_Container);
			helper->add_tracked_numeric_value(speedUnits_ObjPtr, unitKph_OutStr);
			helper->add_tracked_numeric_value(currentSpeedReadout_VarNum, 0);
			helper->add_tracked_numeric_value(currentSpeedMeter_VarNum, 0);
			helper->add_tracked_numeric_value(canAddress_VarNum, 0);
			helper->add_tracked_numeric_value(utAddress_VarNum, 0);
			helper->add_tracked_numeric_value(tcAddress_VarNum, 0);
			helper->add_tracked_numeric_value(tcNumberBoomsSupported_VarNum, 0);
			helper->add_tracked_numeric_value(tcSupportedSections_VarNum, 0);
			helper->add_tracked_numeric_value(tcControlChannels_VarNum, 0);
			helper->add_tracked_numeric_value(tcVersion_VarNum, 0);
			helper->add_tracked_numeric_value(selectedStatisticsContainer_ObjPtr, canStatistics_Container);
			helper->add_tracked_attribute(speed_OutNum, 8, 0.0036f);
		}
		helper->initialize();
		lastConnected = false;
		currentMask = 0xFFFF;
	}

	void TerminalClient::leave()
	{
		std::lock_guard<std::mutex> session(sessionMutex);
		if (!client)
		{
			return;
		}
		events.emit("vt", { { "phase", "leaving" } });
		std::lock_guard<std::mutex> lock(vtMutex);
		client->terminate();
		events.emit("vt", { { "phase", "clientStopped" } });
		helper.reset();
		client.reset();
		if (vt)
		{
			isobus::CANNetworkManager::CANNetwork.deactivate_control_function(vt);
			vt.reset();
		}
		if (lastConnected)
		{
			lastConnected = false;
			events.emit("vt", { { "connected", false }, { "pool", true }, { "left", true } });
		}
	}

	void TerminalClient::rejoin(std::shared_ptr<isobus::InternalControlFunction> ecuToUse)
	{
		std::lock_guard<std::mutex> session(sessionMutex);
		if (pool.empty())
		{
			return;
		}
		ecu = ecuToUse;
		std::lock_guard<std::mutex> lock(vtMutex);
		connect();
	}

	bool TerminalClient::reupload(const std::string &label, std::size_t padBytes)
	{
		if (!client)
		{
			return false;
		}
		uploaded = pool;
		if (padBytes > 0)
		{
			// Padding: number variable objects nothing refers to, 7 bytes each, IDs from 28000 up (the base pool stops at 27010)
			std::uint16_t id = 28000;
			for (std::size_t added = 0; added < padBytes && id < 65000; added += 7, id++)
			{
				uploaded.push_back(static_cast<std::uint8_t>(id & 0xFF));
				uploaded.push_back(static_cast<std::uint8_t>(id >> 8));
				uploaded.push_back(21); // NumberVariable
				for (int i = 0; i < 4; i++)
				{
					uploaded.push_back(0);
				}
			}
		}
		versionLabel = label.empty() ? isobus::IOPFileInterface::hash_object_pool_to_version(uploaded) : label;
		events.emit("vt", { { "connected", false }, { "pool", true }, { "reupload", true }, { "bytes", uploaded.size() }, { "version", versionLabel } });
		leave();
		std::lock_guard<std::mutex> session(sessionMutex);
		std::lock_guard<std::mutex> lock(vtMutex);
		connect();
		return true;
	}

	void TerminalClient::stop()
	{
		if (client)
		{
			client->terminate();
		}
	}

	std::vector<std::string> TerminalClient::alarm_names() const
	{
		std::vector<std::string> names;
		for (const auto &a : alarms)
		{
			names.push_back(a.name);
		}
		return names;
	}

	void TerminalClient::on_key(const VTC::VTKeyEvent &event, const char *source)
	{
		events.emit("key", { { "id", event.objectID }, { "parent", event.parentObjectID }, { "number", event.keyNumber }, { "code", code_name(event.keyEvent) }, { "source", source } });
		if (!behaviour)
		{
			return;
		}
		if (event.keyNumber == 0 && event.keyEvent == VTC::KeyActivationCode::ButtonUnlatchedOrReleased)
		{
			acknowledge_first_alarm(); // key number 0: the terminal's alarm acknowledge
		}
		if (event.keyEvent == VTC::KeyActivationCode::ButtonUnlatchedOrReleased)
		{
			act_on_key(event.objectID);
		}
	}

	void TerminalClient::press_key(std::uint16_t objectId)
	{
		events.emit("key", { { "id", objectId }, { "code", "released" }, { "source", "command" } });
		if (behaviour)
		{
			act_on_key(objectId);
		}
	}

	void TerminalClient::act_on_key(std::uint16_t objectId)
	{
		switch (objectId)
		{
			case home_Key:
				wantedMask = mainRunscreen_DataMask;
				show_mask(mainRunscreen_DataMask);
				break;
			case settings_Key:
				wantedMask = settingsRunscreen_DataMask;
				show_mask(settingsRunscreen_DataMask);
				break;
			case statistics_Key:
				wantedMask = statisticsRunscreen_DataMask;
				show_mask(statisticsRunscreen_DataMask);
				break;
			case alarms_Key:
				wantedMask = alarmsRunscreen_DataMask;
				show_mask(alarmsRunscreen_DataMask);
				break;
			case acknowledgeAlarm_SoftKey:
				acknowledge_first_alarm();
				break;
			case autoManualToggle_Button:
			{
				std::lock_guard<std::mutex> lock(modelMutex);
				const bool wanted = !model.boom(0).autoMode;
				for (std::size_t b = 0; b < model.boom_count(); b++)
				{
					model.set_auto(b, wanted);
				}
				break;
			}
			case section1Toggle_Button:
			case section2Toggle_Button:
			case section3Toggle_Button:
			case section4Toggle_Button:
			case section5Toggle_Button:
			case section6Toggle_Button:
			{
				std::size_t rest = objectId - section1Toggle_Button;
				std::lock_guard<std::mutex> lock(modelMutex);
				for (std::size_t b = 0; b < model.boom_count(); b++)
				{
					if (rest < model.boom(b).sections.size())
					{
						model.set_switch(b, rest, !model.section(b, rest).sw);
						break;
					}
					rest -= model.boom(b).sections.size();
				}
				break;
			}
			default:
				break;
		}
	}

	void TerminalClient::on_value(const VTC::VTChangeNumericValueEvent &event)
	{
		events.emit("value", { { "id", event.objectID }, { "value", event.value } });
		if (!behaviour)
		{
			return;
		}
		if (event.objectID == statisticsSelection_VarNum)
		{
			std::uint16_t container = UNDEFINED;
			switch (event.value)
			{
				case 1:
					container = canStatistics_Container;
					break;
				case 2:
					container = utStatistics_Container;
					break;
				case 3:
					container = tcStatistics_Container;
					break;
				case 4:
					container = credits_Container;
					break;
				default:
					break;
			}
			std::lock_guard<std::mutex> lock(vtMutex);
			helper->set_numeric_value(selectedStatisticsContainer_ObjPtr, container);
		}
		else if (event.objectID == enableAlarms_VarNum)
		{
			alarmsEnabled = event.value != 0;
		}
	}

	void TerminalClient::on_mask(const VTC::VTChangeActiveMaskEvent &event)
	{
		currentMask = event.maskObjectID;
		events.emit("mask", { { "mask", event.maskObjectID }, { "parent", event.parentObjectID }, { "errorObject", event.errorObjectID }, { "missingObjects", event.missingObjects }, { "errors", event.maskOrChildHasErrors || event.anyOtherError }, { "poolDeleted", event.poolDeleted } });
	}

	void TerminalClient::on_aux(const VTC::AuxiliaryFunctionEvent &event)
	{
		// An assigned input moved (docs/PLAN.md phase 7). value1 is the state or position, value2 the transition count or 0xFFFF.
		events.emit("aux", { { "function", event.function.functionObjectID }, { "input", event.function.inputObjectID }, { "type", static_cast<int>(event.function.functionType) }, { "value1", event.value1 }, { "value2", event.value2 } });
		if (!auxPool)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(modelMutex);
		switch (event.function.functionObjectID)
		{
			case AUXN_FUNCTION_ONOFF: // a latching on/off: the master
				for (std::size_t b = 0; b < model.boom_count(); b++)
				{
					model.set_master(b, event.value1 != 0);
				}
				break;
			case AUXN_FUNCTION_ONOFF_TOGGLE: // each press toggles auto/manual
				if (event.value1 != 0)
				{
					const bool wanted = !model.boom(0).autoMode;
					for (std::size_t b = 0; b < model.boom_count(); b++)
					{
						model.set_auto(b, wanted);
					}
				}
				break;
			case AUXN_FUNCTION_PLUS: // a press steps the operator's rate up by a tenth
				if (event.value1 != 0 && model.product_count() > 0)
				{
					model.set_operator_rate(0, model.product(0).operatorRate * 1.1);
				}
				break;
			default:
				break;
		}
	}

	void TerminalClient::show_mask(std::uint16_t maskId)
	{
		// Sent directly, not through the update helper: the helper skips the command when the
		// terminal's 1 Hz status message still names the mask it is asked for, which after an
		// alarm acknowledge is exactly the case, and the screen would stay on the alarm.
		std::lock_guard<std::mutex> lock(vtMutex);
		if (client && client->get_is_connected())
		{
			client->send_change_active_mask(example_WorkingSet, maskId);
			currentMask = maskId;
		}
	}

	bool TerminalClient::change_mask(std::uint16_t maskId)
	{
		if (!client)
		{
			return false;
		}
		wantedMask = maskId;
		show_mask(maskId);
		return true;
	}

	bool TerminalClient::set_value(std::uint16_t objectId, std::uint32_t value)
	{
		if (!client)
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(vtMutex);
		return client->send_change_numeric_value(objectId, value);
	}

	bool TerminalClient::raise_alarm(const std::string &name, std::string &error)
	{
		for (auto &a : alarms)
		{
			if (a.name == name)
			{
				a.forced = true;
				a.acknowledged = false;
				return true;
			}
		}
		error = "no alarm " + name + "; the alarms are";
		for (const auto &a : alarms)
		{
			error += " " + a.name;
		}
		return false;
	}

	void TerminalClient::clear_alarms()
	{
		for (auto &a : alarms)
		{
			a.forced = false;
			if (a.active)
			{
				a.acknowledged = true;
			}
		}
	}

	void TerminalClient::acknowledge_first_alarm()
	{
		for (auto &a : alarms)
		{
			if (a.active && !a.acknowledged)
			{
				a.acknowledged = true;
				a.forced = false;
				events.emit("alarm", { { "name", a.name }, { "state", "acknowledged" } });
				break;
			}
		}
	}

	void TerminalClient::update_alarms()
	{
		const std::uint32_t now = events.now_ms();
		bool speedKnown, connected = tc.connected();
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			speedKnown = model.speed_known();
		}
		bool anyActive = false;
		for (auto &a : alarms)
		{
			const bool cause = (a.name == "noSpeed") ? !speedKnown : !connected;
			if (cause && !a.condition)
			{
				a.sinceMs = now;
			}
			a.condition = cause;
			const bool due = alarmsEnabled && cause && (now - a.sinceMs) >= static_cast<std::uint32_t>(a.afterSeconds * 1000.0);
			const bool shouldBeActive = a.forced || due;
			if (shouldBeActive && !a.active)
			{
				a.active = true;
				a.acknowledged = false;
				events.emit("alarm", { { "name", a.name }, { "state", "raised" }, { "forced", a.forced } });
			}
			else if (!shouldBeActive && a.active)
			{
				a.active = false;
				a.acknowledged = false;
				events.emit("alarm", { { "name", a.name }, { "state", "cleared" } });
			}
			if (a.active && !a.acknowledged)
			{
				if (!anyActive && currentMask != a.mask)
				{
					show_mask(a.mask);
				}
				anyActive = true;
			}
		}
		if (!anyActive && currentMask != wantedMask && (currentMask == noSpeed_AlarmMask || currentMask == noTaskController_AlarmMask))
		{
			show_mask(wantedMask); // every alarm acknowledged or gone: back to the page the operator had
		}
	}

	void TerminalClient::update_screen()
	{
		std::lock_guard<std::mutex> vtLock(vtMutex);
		std::lock_guard<std::mutex> lock(modelMutex);
		const std::uint16_t pointers[] = { section1EnableState_ObjPtr, section2EnableState_ObjPtr, section3EnableState_ObjPtr, section4EnableState_ObjPtr, section5EnableState_ObjPtr, section6EnableState_ObjPtr };
		const std::uint16_t rects[] = { section1Status_OutRect, section2Status_OutRect, section3Status_OutRect, section4Status_OutRect, section5Status_OutRect, section6Status_OutRect };
		std::size_t i = 0;
		for (std::size_t b = 0; b < model.boom_count() && i < 6; b++)
		{
			for (std::size_t s = 0; s < model.boom(b).sections.size() && i < 6; s++, i++)
			{
				const auto &sec = model.section(b, s);
				helper->set_numeric_value(pointers[i], sec.sw ? onButtonSliderSmall_OutPict : offButtonSliderSmall_OutPict);
				helper->set_attribute(rects[i], 5, static_cast<std::uint32_t>(sec.actual ? solidGreen_FillAttr : (sec.setpoint ? solidYellow_FillAttr : solidRed_FillAttr)));
			}
		}
		helper->set_numeric_value(autoManual_ObjPtr, model.boom(0).autoMode ? autoMode_Container : manualMode_Container);
		const double mmPerS = model.speed_known() ? model.speed_mm_s() : 0.0;
		helper->set_numeric_value(currentSpeedReadout_VarNum, static_cast<std::uint32_t>(mmPerS + 0.5));
		helper->set_numeric_value(currentSpeedMeter_VarNum, static_cast<std::uint32_t>(std::min(30.0, mmPerS * 0.0036) + 0.5));
		helper->set_numeric_value(canAddress_VarNum, static_cast<std::uint32_t>(tc.address() < 0 ? 0 : tc.address()));
		helper->set_numeric_value(utAddress_VarNum, vt ? vt->get_address() : 0);
		helper->set_numeric_value(tcAddress_VarNum, tc.tc_address());
		helper->set_numeric_value(tcNumberBoomsSupported_VarNum, tc.tc_booms());
		helper->set_numeric_value(tcSupportedSections_VarNum, tc.tc_sections());
		helper->set_numeric_value(tcControlChannels_VarNum, tc.tc_channels());
		helper->set_numeric_value(tcVersion_VarNum, tc.tc_version());
	}

	void TerminalClient::update()
	{
		std::lock_guard<std::mutex> session(sessionMutex);
		if (!client)
		{
			return;
		}
		const bool connected = client->get_is_connected();
		if (connected != lastConnected)
		{
			lastConnected = connected;
			nlohmann::json fields = { { "connected", connected }, { "pool", true } };
			if (connected)
			{
				fields["version"] = static_cast<int>(client->get_connected_vt_version());
				fields["address"] = vt->get_address();
				fields["label"] = versionLabel;
			}
			events.emit("vt", fields);
			if (connected && behaviour)
			{
				show_mask(wantedMask);
			}
		}
		if (!connected || !behaviour)
		{
			return;
		}
		update_alarms();
		const std::uint32_t now = events.now_ms();
		if (now - lastScreenMs >= 100)
		{
			lastScreenMs = now;
			update_screen();
		}
	}
}
