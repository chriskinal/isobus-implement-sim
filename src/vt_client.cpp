#include "vt_client.hpp"
#include "tc_client.hpp"

#include "../pools/aux_functions_ids.hpp"
#include "../pools/base_pool_ids.hpp"
#include "../pools/spreader_ids.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/utility/iop_file_interface.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace sim
{
	// The first 7 characters of the stack's 16-character pool hash. A label longer than 7 makes
	// the stack's client ask the terminal with Extended Get Versions (VT version 5, 32-byte
	// labels), which AgIsoVirtualTerminal on the bench never answered, and the client then
	// resets forever instead of uploading (PLAN.md phase 3 record). 7 bytes is the label every
	// terminal from version 3 on stores.
	// The terminal's VT version as its number (2 to 6), 0 when unknown; the enum's index is not it
	static int vt_version_number(isobus::VirtualTerminalClient::VTVersion v)
	{
		using V = isobus::VirtualTerminalClient::VTVersion;
		switch (v)
		{
			case V::Version2OrOlder: return 2;
			case V::Version3: return 3;
			case V::Version4: return 4;
			case V::Version5: return 5;
			case V::Version6: return 6;
			default: return 0;
		}
	}

	static std::string short_version_label(const std::vector<std::uint8_t> &pool)
	{
		return isobus::IOPFileInterface::hash_object_pool_to_version(pool.data(), pool.size()).substr(0, 7);
	}

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
		versionLabel = short_version_label(pool);
		const auto slash = poolPath.find_last_of('/');
		const std::string file = poolPath.substr(slash == std::string::npos ? 0 : slash + 1);
		spreaderPool = file == "spreader.iop";
		behaviour = file == "base.iop" || spreaderPool;
		auxPool = file == "aux_functions.iop";
		if (spreaderPool)
		{
			alarms[0].mask = spreader_pool::speedAlarm_AlarmMask;
			alarms[1].mask = spreader_pool::tcAlarm_AlarmMask;
		}

		uploaded = pool;
		connect();
		wantedMask = spreaderPool ? spreader_pool::run_DataMask : (behaviour ? mainRunscreen_DataMask : 0xFFFF);
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
		if (spreaderPool)
		{
			using namespace spreader_pool;
			const std::uint16_t rects[] = { section1_Rect, section2_Rect, section3_Rect, section4_Rect, section5_Rect, section6_Rect, section7_Rect, section8_Rect };
			for (auto r : rects)
			{
				helper->add_tracked_attribute(r, 5, static_cast<std::uint32_t>(off_Fill));
			}
			helper->add_tracked_attribute(master_Rect, 5, static_cast<std::uint32_t>(off_Fill));
			helper->add_tracked_attribute(mode_Rect, 5, static_cast<std::uint32_t>(off_Fill));
			helper->add_tracked_numeric_value(tcStatus_ObjPtr, tcOffline_OutStr);
			helper->add_tracked_numeric_value(masterStatus_ObjPtr, masterOff_OutStr);
			helper->add_tracked_numeric_value(modeStatus_ObjPtr, modeManual_OutStr);
			helper->add_tracked_numeric_value(prescription_ObjPtr, rateFromOperator_OutStr);
			for (std::uint16_t n : { targetRate_OutNum, actualRate_OutNum, groundSpeed_OutNum, binLevel_OutNum, binLevel_BarGraph, area_OutNum, totalArea_OutNum, applied_OutNum, lifetime_OutNum, distance_OutNum, workTime_OutNum })
			{
				helper->add_tracked_numeric_value(n, 0);
			}
		}
		else if (behaviour)
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
		versionLabel = label.empty() ? short_version_label(uploaded) : label;
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
		if (spreaderPool)
		{
			act_on_spreader_key(objectId);
			return;
		}
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

	// The spreader pool's AUX-N functions: the buttons do what the soft keys do, the trim moves the
	// operator's rate while the stick is held over (apply_rate_trim)
	void TerminalClient::on_spreader_aux(const VTC::AuxiliaryFunctionEvent &event)
	{
		using namespace spreader_pool;
		const std::uint16_t function = event.function.functionObjectID;
		if (function == rateTrim_AuxFunction)
		{
			trimPosition = event.value1;
			trimHeardMs = events.now_ms();
			return;
		}
		// A non-latching button reports 0 released, 1 pressed, 2 held, and the input device
		// repeats its status every second: act on the press only
		const bool down = event.value1 != 0 && event.value1 != 0xFFFF;
		bool &was = auxDown[function];
		const bool pressed = down && !was;
		was = down;
		if (!pressed)
		{
			return;
		}
		switch (function)
		{
			case master_AuxFunction:
				act_on_spreader_key(master_Key);
				break;
			case autoManual_AuxFunction:
				act_on_spreader_key(autoManual_Key);
				break;
			case ratePlus_AuxFunction:
				act_on_spreader_key(ratePlus_Key);
				break;
			case rateMinus_AuxFunction:
				act_on_spreader_key(rateMinus_Key);
				break;
			case rateReset_AuxFunction:
				act_on_spreader_key(rateReset_Key);
				break;
			default:
				break;
		}
	}

	// The trim, every update: the stick's deflection from the middle moves the operator's rate by
	// up to a quarter a second, taking it from the task controller as the rate keys do. A small
	// deflection does nothing, and an input not heard from for 2.5 s (it repeats every second)
	// counts as let go.
	void TerminalClient::apply_rate_trim()
	{
		constexpr double MIDDLE = 32127.0, DEADBAND = 0.05, PER_SECOND = 0.25;
		const std::uint32_t now = events.now_ms();
		const double dt = std::min(0.5, (now - lastTrimMs) / 1000.0);
		lastTrimMs = now;
		double deflection = std::max(-1.0, std::min(1.0, (trimPosition - MIDDLE) / MIDDLE));
		if (trimHeardMs == 0 || now - trimHeardMs > 2500 || std::abs(deflection) < DEADBAND)
		{
			deflection = 0;
		}
		if ((deflection != 0) != trimming)
		{
			trimming = deflection != 0;
			events.emit("trim", { { "active", trimming } });
		}
		if (!trimming)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(modelMutex);
		for (std::size_t p = 0; p < model.product_count(); p++)
		{
			const double from = model.product(p).prescription ? model.target_rate(p) : model.product(p).operatorRate;
			model.set_prescription(p, false);
			model.set_operator_rate(p, from * (1.0 + PER_SECOND * deflection * dt));
		}
	}

	void TerminalClient::act_on_spreader_key(std::uint16_t objectId)
	{
		using namespace spreader_pool;
		switch (objectId)
		{
			case totals_Key:
				wantedMask = totals_DataMask;
				show_mask(totals_DataMask);
				return;
			case run_Key:
				wantedMask = run_DataMask;
				show_mask(run_DataMask);
				return;
			case acknowledge_Key:
				acknowledge_first_alarm();
				return;
			default:
				break;
		}
		std::lock_guard<std::mutex> lock(modelMutex);
		switch (objectId)
		{
			case master_Key:
			{
				const bool wanted = !model.boom(0).master;
				for (std::size_t b = 0; b < model.boom_count(); b++)
				{
					model.set_master(b, wanted);
				}
				break;
			}
			case autoManual_Key:
			{
				const bool wanted = !model.boom(0).autoMode;
				for (std::size_t b = 0; b < model.boom_count(); b++)
				{
					model.set_auto(b, wanted);
				}
				break;
			}
			// The rate keys hand the rate to the operator, as a Raven's rate buttons do: prescription
			// control off (DDI 158 = 0), which the task controller can set back
			case ratePlus_Key: // steps of a tenth
			case rateMinus_Key:
				for (std::size_t p = 0; p < model.product_count(); p++)
				{
					const double from = model.product(p).prescription ? model.target_rate(p) : model.product(p).operatorRate;
					model.set_prescription(p, false);
					model.set_operator_rate(p, from * (objectId == ratePlus_Key ? 1.1 : 1.0 / 1.1));
				}
				break;
			case rateReset_Key:
				for (std::size_t p = 0; p < model.product_count(); p++)
				{
					model.set_prescription(p, false);
					model.set_operator_rate(p, profile.products[p].rate);
				}
				break;
			case resetTotals_Key:
				for (std::size_t p = 0; p < model.product_count(); p++)
				{
					model.reset_trip_total(p);
				}
				for (std::size_t b = 0; b < model.boom_count(); b++)
				{
					model.reset_boom_totals(b);
				}
				break;
			default:
				break;
		}
	}

	void TerminalClient::on_value(const VTC::VTChangeNumericValueEvent &event)
	{
		events.emit("value", { { "id", event.objectID }, { "value", event.value } });
		if (!behaviour || spreaderPool)
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
		if (spreaderPool)
		{
			on_spreader_aux(event);
			return;
		}
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
			client->send_change_active_mask(spreaderPool ? spreader_pool::spreader_WorkingSet : example_WorkingSet, maskId);
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
		const bool onAlarmMask = std::any_of(alarms.begin(), alarms.end(), [this](const Alarm &a) { return a.mask == currentMask; });
		if (!anyActive && currentMask != wantedMask && onAlarmMask)
		{
			show_mask(wantedMask); // every alarm acknowledged or gone: back to the page the operator had
		}
	}

	void TerminalClient::lay_out_sections()
	{
		// The bar has eight slots; the profile's sections share its width, and the slots left over shrink to nothing
		using namespace spreader_pool;
		const std::uint16_t rects[] = { section1_Rect, section2_Rect, section3_Rect, section4_Rect, section5_Rect, section6_Rect, section7_Rect, section8_Rect };
		const std::size_t shown = std::min<std::size_t>(8, std::max<std::size_t>(1, profile.total_sections()));
		const std::uint16_t width = static_cast<std::uint16_t>(460 / shown);
		std::lock_guard<std::mutex> lock(vtMutex);
		for (std::size_t i = 0; i < 8; i++)
		{
			if (i < shown)
			{
				client->send_change_size_command(rects[i], static_cast<std::uint16_t>(width - 4), 50);
				client->send_change_child_position(rects[i], sections_Container, static_cast<std::uint16_t>(i * width), 0);
			}
			else
			{
				client->send_change_size_command(rects[i], 0, 0);
			}
		}
	}

	void TerminalClient::update_spreader_screen()
	{
		using namespace spreader_pool;
		const bool tcConnected = tc.connected();
		std::lock_guard<std::mutex> vtLock(vtMutex);
		std::lock_guard<std::mutex> lock(modelMutex);
		const std::uint16_t rects[] = { section1_Rect, section2_Rect, section3_Rect, section4_Rect, section5_Rect, section6_Rect, section7_Rect, section8_Rect };
		std::size_t i = 0;
		double areaM2 = 0, distanceMm = 0, timeS = 0;
		for (std::size_t b = 0; b < model.boom_count(); b++)
		{
			for (std::size_t s = 0; s < model.boom(b).sections.size() && i < 8; s++, i++)
			{
				const auto &sec = model.section(b, s);
				const std::uint16_t fill = sec.error ? error_Fill : (sec.actual ? on_Fill : (sec.commanded ? pending_Fill : off_Fill));
				helper->set_attribute(rects[i], 5, static_cast<std::uint32_t>(fill));
			}
			areaM2 += model.boom(b).areaM2;
			distanceMm += model.boom(b).effectiveDistanceMm;
			timeS += model.boom(b).effectiveTimeS;
		}
		const auto &boom = model.boom(0);
		helper->set_attribute(master_Rect, 5, static_cast<std::uint32_t>(boom.master ? on_Fill : off_Fill));
		helper->set_numeric_value(masterStatus_ObjPtr, boom.master ? masterOn_OutStr : masterOff_OutStr);
		helper->set_attribute(mode_Rect, 5, static_cast<std::uint32_t>(boom.autoMode ? on_Fill : off_Fill));
		helper->set_numeric_value(modeStatus_ObjPtr, boom.autoMode ? modeAuto_OutStr : modeManual_OutStr);
		helper->set_numeric_value(tcStatus_ObjPtr, tcConnected ? tcOnline_OutStr : tcOffline_OutStr);
		auto whole = [](double v) { return static_cast<std::uint32_t>(std::max(0.0, v) + 0.5); };
		if (model.product_count() > 0)
		{
			const auto &p = model.product(0);
			const auto &pp = profile.products[0];
			helper->set_numeric_value(prescription_ObjPtr, (p.prescription && tcConnected) ? rateFromTc_OutStr : rateFromOperator_OutStr);
			helper->set_numeric_value(targetRate_OutNum, whole(model.target_rate(0)));
			helper->set_numeric_value(actualRate_OutNum, whole(p.actualRate));
			helper->set_numeric_value(binLevel_OutNum, whole(p.level));
			helper->set_numeric_value(binLevel_BarGraph, whole(pp.bin.capacity > 0 ? 100.0 * p.level / pp.bin.capacity : 0));
			helper->set_numeric_value(applied_OutNum, whole(p.tripTotal));
			helper->set_numeric_value(lifetime_OutNum, whole(p.lifetimeTotal));
		}
		helper->set_numeric_value(groundSpeed_OutNum, whole(model.speed_known() ? model.speed_mm_s() : 0.0));
		helper->set_numeric_value(area_OutNum, whole(areaM2));
		helper->set_numeric_value(totalArea_OutNum, whole(areaM2));
		helper->set_numeric_value(distance_OutNum, whole(distanceMm / 1000.0));
		helper->set_numeric_value(workTime_OutNum, whole(timeS));
	}

	void TerminalClient::update_screen()
	{
		if (spreaderPool)
		{
			update_spreader_screen();
			return;
		}
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
				fields["version"] = vt_version_number(client->get_connected_vt_version());
				fields["address"] = vt->get_address();
				fields["label"] = versionLabel;
			}
			events.emit("vt", fields);
			if (connected && spreaderPool)
			{
				lay_out_sections();
			}
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
		if (spreaderPool)
		{
			apply_rate_trim();
		}
		const std::uint32_t now = events.now_ms();
		if (now - lastScreenMs >= 100)
		{
			lastScreenMs = now;
			update_screen();
		}
	}
}
