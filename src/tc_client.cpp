#include "tc_client.hpp"
#include "units.hpp"

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/isobus_standard_data_description_indices.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace sim
{
	using DDI = isobus::DataDescriptionIndex;

	namespace
	{
		constexpr std::uint16_t d(DDI x)
		{
			return static_cast<std::uint16_t>(x);
		}

		const char *state_name(isobus::TaskControllerClient::StateMachineState s)
		{
			using S = isobus::TaskControllerClient::StateMachineState;
			switch (s)
			{
				case S::Disconnected:
					return "disconnected";
				case S::WaitForStartUpDelay:
					return "startupDelay";
				case S::WaitForServerStatusMessage:
					return "waitingForServer";
				case S::SendWorkingSetMaster:
				case S::SendStatusMessage:
					return "workingSetMaster";
				case S::RequestVersion:
				case S::WaitForRequestVersionResponse:
				case S::WaitForRequestVersionFromServer:
				case S::SendRequestVersionResponse:
					return "version";
				case S::RequestLanguage:
				case S::WaitForLanguageResponse:
					return "language";
				case S::ProcessDDOP:
					return "processDdop";
				case S::RequestStructureLabel:
				case S::WaitForStructureLabelResponse:
				case S::RequestLocalizationLabel:
				case S::WaitForLocalizationLabelResponse:
					return "labels";
				case S::SendDeleteObjectPool:
				case S::WaitForDeleteObjectPoolResponse:
					return "deletePool";
				case S::SendRequestTransferObjectPool:
				case S::WaitForRequestTransferObjectPoolResponse:
				case S::BeginTransferDDOP:
				case S::WaitForDDOPTransfer:
				case S::WaitForObjectPoolTransferResponse:
					return "uploadingDdop";
				case S::SendObjectPoolActivate:
				case S::WaitForObjectPoolActivateResponse:
					return "activatingDdop";
				case S::Connected:
					return "connected";
				case S::DeactivateObjectPool:
				case S::WaitForObjectPoolDeactivateResponse:
					return "deactivating";
			}
			return "?";
		}
	}

	ImplementClient::ImplementClient(const Profile &p, Model &m, std::mutex &mx, Events &ev) :
	  profile(p),
	  model(m),
	  modelMutex(mx),
	  events(ev)
	{
		lastSetpoint.resize(p.booms.size());
		lastCommanded.resize(p.booms.size());
		lastActual.resize(p.booms.size());
		lastAuto.resize(p.booms.size(), true);
		lastMaster.resize(p.booms.size(), true);
		lastTarget.resize(p.products.size(), -1.0);
	}

	bool ImplementClient::start(std::shared_ptr<isobus::CANHardwarePlugin> driver)
	{
		isobus::CANHardwareInterface::set_number_of_can_channels(1);
		isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, driver);
		if (!isobus::CANHardwareInterface::start() || !driver->get_is_valid())
		{
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));

		name = isobus::NAME(0);
		name.set_arbitrary_address_capable(true);
		name.set_industry_group(2);
		name.set_device_class(profile.type == "sprayer" ? 6 : 4); // ISO 11783-5 IG2: 4 planters and seeders, 6 sprayers
		name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::RateControl));
		name.set_identity_number(profile.identity);
		name.set_manufacturer_code(1407); // PLAN.md open item 1
		name.set_ecu_instance(0);
		name.set_function_instance(0);
		name.set_device_class_instance(0);

		pool = build_ddop(profile, name, "0.1.0");

		connect();

		std::vector<std::uint8_t> binary;
		pool.pool->generate_binary_object_pool(binary);
		events.emit("profile", { { "name", profile.name }, { "type", profile.type }, { "booms", profile.booms.size() }, { "sections", profile.total_sections() }, { "products", profile.products.size() }, { "sectionControl", to_string(profile.sectionControl) }, { "ddopObjects", pool.objectCount }, { "ddopBytes", binary.size() }, { "structureLabel", pool.structureLabel } });
		return true;
	}

	void ImplementClient::connect()
	{
		using P = isobus::NAME::NAMEParameters;
		const std::vector<isobus::NAMEFilter> tcFilters = {
			isobus::NAMEFilter(P::FunctionCode, static_cast<std::uint8_t>(isobus::NAME::Function::TaskController)),
			isobus::NAMEFilter(P::FunctionInstance, 0),
			isobus::NAMEFilter(P::IndustryGroup, static_cast<std::uint8_t>(isobus::NAME::IndustryGroup::AgriculturalAndForestryEquipment)),
			isobus::NAMEFilter(P::DeviceClass, static_cast<std::uint8_t>(isobus::NAME::DeviceClass::NonSpecific))
		};
		ecu = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0);
		tc = isobus::CANNetworkManager::CANNetwork.create_partnered_control_function(0, tcFilters);
		client = std::make_shared<isobus::TaskControllerClient>(tc, ecu, nullptr);
		client->configure(pool.pool,
		                  static_cast<std::uint8_t>(profile.booms.size()),
		                  static_cast<std::uint8_t>(std::min<std::size_t>(profile.total_sections(), 255)),
		                  1, // channels for position based control
		                  true, // documentation
		                  false, // TC-GEO without position based control
		                  true, // TC-GEO with position based control
		                  false, // peer control
		                  true); // section control
		client->add_request_value_callback(request_value_callback, this);
		client->add_value_command_callback(value_command_callback, this);
		client->add_default_process_data_requested_callback(default_process_data_callback, this);
		client->initialize(true);

		speed = std::make_unique<isobus::SpeedMessagesInterface>(ecu, false, false, false, false);
		speed->get_machine_selected_speed_data_event_publisher().add_listener([this](const std::shared_ptr<isobus::MachineSelectedSpeedData> data, bool) { on_speed(0, data->get_machine_speed()); });
		speed->get_ground_based_machine_speed_data_event_publisher().add_listener([this](const std::shared_ptr<isobus::GroundBasedSpeedData> data, bool) { on_speed(1, data->get_machine_speed()); });
		speed->get_wheel_based_machine_speed_data_event_publisher().add_listener([this](const std::shared_ptr<isobus::WheelBasedMachineSpeedData> data, bool) { on_speed(2, data->get_machine_speed()); });
		speed->initialize();
	}

	std::shared_ptr<isobus::InternalControlFunction> ImplementClient::restart()
	{
		std::lock_guard<std::mutex> session(sessionMutex);
		restarts++;
		events.emit("restart", { { "count", restarts }, { "phase", "leaving" } });
		if (client)
		{
			client->terminate();
			client.reset();
		}
		events.emit("restart", { { "count", restarts }, { "phase", "clientStopped" } });
		speed.reset();
		if (ecu)
		{
			isobus::CANNetworkManager::CANNetwork.deactivate_control_function(ecu);
			ecu.reset();
		}
		if (tc)
		{
			isobus::CANNetworkManager::CANNetwork.deactivate_control_function(tc);
			tc.reset();
		}
		events.emit("restart", { { "count", restarts }, { "phase", "left" } });
		lastConnected = false;
		lastState = -1;
		lastAddress = 0xFF;
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		connect();
		events.emit("restart", { { "count", restarts }, { "phase", "rejoining" } });
		return ecu;
	}

	bool ImplementClient::reupload_bad_ddop()
	{
		std::lock_guard<std::mutex> session(sessionMutex);
		if (!client)
		{
			return false;
		}
		std::vector<std::uint8_t> binary;
		pool.pool->generate_binary_object_pool(binary);
		if (binary.size() < 64)
		{
			return false;
		}
		// Cut the tail off and scramble a stretch in the middle: object types and IDs no parser accepts
		binary.resize(binary.size() - 40);
		for (std::size_t i = binary.size() / 2; i < binary.size() / 2 + 24; i++)
		{
			binary[i] = static_cast<std::uint8_t>(0xA5 ^ i);
		}
		events.emit("ddop", { { "what", "bad" }, { "bytes", binary.size() } });
		return client->reupload_device_descriptor_object_pool(binary.data(), static_cast<std::uint32_t>(binary.size()));
	}

	bool ImplementClient::claim_conflict(std::uint8_t address)
	{
		std::lock_guard<std::mutex> session(sessionMutex);
		if (address == 0 && tc && tc->get_address_valid())
		{
			address = tc->get_address();
		}
		if (address == 0 || address >= 0xFE)
		{
			return false;
		}
		// An address claim (PGN 0xEE00, priority 6) from that address by a contender: our NAME with
		// another identity number, so it is a second device and not us moving
		isobus::CANMessageFrame frame;
		frame.identifier = 0x18EEFF00u | address;
		frame.isExtendedFrame = true;
		frame.dataLength = 8;
		frame.channel = 0;
		isobus::NAME contender = name;
		contender.set_identity_number((profile.identity + 1000) & 0x1FFFFF);
		const std::uint64_t raw = contender.get_full_name();
		for (int i = 0; i < 8; i++)
		{
			frame.data[i] = static_cast<std::uint8_t>(raw >> (8 * i));
		}
		events.emit("conflict", { { "address", address }, { "contenderIdentity", (profile.identity + 1000) & 0x1FFFFF } });
		return isobus::CANHardwareInterface::transmit_can_frame(frame);
	}

	void ImplementClient::stop()
	{
		if (client)
		{
			client->terminate();
		}
		isobus::CANHardwareInterface::stop();
	}

	void ImplementClient::on_speed(int source, std::uint32_t mmPerSecond)
	{
		// The best source present wins: machine selected, then ground based, then wheel based
		const bool haveMss = speed->get_number_received_machine_selected_speed_sources() > 0;
		const bool haveGround = speed->get_number_received_ground_based_speed_sources() > 0;
		const bool take = (source == 0) || (source == 1 && !haveMss) || (source == 2 && !haveMss && !haveGround);
		if (take)
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			model.set_speed(mmPerSecond, events.now_ms());
		}
	}

	bool ImplementClient::request_value_callback(std::uint16_t element, std::uint16_t ddi, std::int32_t &value, void *parent)
	{
		return static_cast<ImplementClient *>(parent)->request_value(element, ddi, value);
	}

	bool ImplementClient::value_command_callback(std::uint16_t element, std::uint16_t ddi, std::int32_t value, void *parent)
	{
		static_cast<ImplementClient *>(parent)->value_command(element, ddi, value);
		return true;
	}

	bool ImplementClient::default_process_data_callback(std::uint16_t element, std::uint16_t ddi, isobus::TaskControllerClient::DefaultProcessDataSettings &settings, void *parent)
	{
		auto *self = static_cast<ImplementClient *>(parent);
		// MODEL.md section 7: states and modes on change; rates, contents and pressures on change and every second; totals as totals
		bool handled = true;
		switch (ddi)
		{
			case d(DDI::ActualWorkState):
			case d(DDI::SetpointWorkState):
			case d(DDI::SectionControlState):
			case d(DDI::ActualWorkingWidth):
				settings.enableChangeThresholdTrigger = true;
				settings.changeThreshold = 1;
				break;
			case d(DDI::ActualVolumePerAreaApplicationRate):
			case d(DDI::ActualMassPerAreaApplicationRate):
			case d(DDI::ActualCountPerAreaApplicationRate):
			case d(DDI::SetpointVolumePerAreaApplicationRate):
			case d(DDI::SetpointMassPerAreaApplicationRate):
			case d(DDI::SetpointCountPerAreaApplicationRate):
			case d(DDI::ActualVolumeContent):
			case d(DDI::ActualMassContent):
			case d(DDI::ActualCountContent):
			case d(DDI::MaximumVolumeContent):
			case d(DDI::MaximumMassContent):
			case d(DDI::MaximumCountContent):
			case d(DDI::ActualProductPressure):
			case d(DDI::ActualVolumePerTimeApplicationRate):
			case d(DDI::PrescriptionControlState):
				settings.enableChangeThresholdTrigger = true;
				settings.changeThreshold = 1;
				settings.enableTimeTrigger = true;
				settings.timeTriggerInterval_ms = 1000;
				break;
			default:
				if (ddi >= d(DDI::ActualCondensedWorkState1_16) && ddi <= d(DDI::ActualCondensedWorkState241_256))
				{
					settings.enableChangeThresholdTrigger = true;
					settings.changeThreshold = 1;
				}
				else
				{
					handled = false;
				}
				break;
		}
		self->events.emit("defaults", { { "element", element }, { "ddi", ddi }, { "triggers", handled } });
		return handled;
	}

	bool ImplementClient::request_value(std::uint16_t element, std::uint16_t ddi, std::int32_t &value)
	{
		std::lock_guard<std::mutex> lock(modelMutex);
		value = 0;
		std::size_t boom = 0, index = 0, product = 0;
		if (element == pool.map.device)
		{
			if (ddi == d(DDI::ActualWorkState))
			{
				value = model.any_actual_on() ? 1 : 0;
			}
		}
		else if (element == pool.map.connector)
		{
			if (ddi == d(DDI::DeviceElementOffsetX))
			{
				value = profile.connector.x;
			}
			else if (ddi == d(DDI::DeviceElementOffsetY))
			{
				value = profile.connector.y;
			}
		}
		else if (pool.map.boom_for(element, boom))
		{
			const auto &b = model.boom(boom);
			if (ddi == d(DDI::ActualWorkingWidth))
			{
				value = model.working_width_mm(boom);
			}
			else if (ddi == d(DDI::ActualWorkState))
			{
				value = model.any_actual_on(boom) ? 1 : 0;
			}
			else if (ddi == d(DDI::SetpointWorkState))
			{
				value = b.master ? 1 : 0;
			}
			else if (ddi == d(DDI::SectionControlState))
			{
				value = b.autoMode ? 1 : 0;
			}
			else if (ddi == d(DDI::TotalArea))
			{
				value = static_cast<std::int32_t>(b.areaM2);
			}
			else if (ddi == d(DDI::EffectiveTotalDistance))
			{
				value = static_cast<std::int32_t>(b.effectiveDistanceMm);
			}
			else if (ddi == d(DDI::IneffectiveTotalDistance))
			{
				value = static_cast<std::int32_t>(b.ineffectiveDistanceMm);
			}
			else if (ddi == d(DDI::EffectiveTotalTime))
			{
				value = static_cast<std::int32_t>(b.effectiveTimeS);
			}
			else if (ddi == d(DDI::IneffectiveTotalTime))
			{
				value = static_cast<std::int32_t>(b.ineffectiveTimeS);
			}
			else if (ddi >= d(DDI::ActualCondensedWorkState1_16) && ddi <= d(DDI::ActualCondensedWorkState241_256))
			{
				const std::size_t chunk = ddi - d(DDI::ActualCondensedWorkState1_16);
				value = chunk < model.condensed_chunks(boom) ? static_cast<std::int32_t>(model.condensed_actual(boom, chunk)) : -1;
			}
			else if (ddi >= d(DDI::SetpointCondensedWorkState1_16) && ddi <= d(DDI::SetpointCondensedWorkState241_256))
			{
				const std::size_t chunk = ddi - d(DDI::SetpointCondensedWorkState1_16);
				value = chunk < model.condensed_chunks(boom) ? static_cast<std::int32_t>(model.condensed_setpoint(boom, chunk)) : -1;
			}
		}
		else if (pool.map.section_for(element, boom, index))
		{
			const auto &s = model.section(boom, index);
			if (ddi == d(DDI::ActualWorkState))
			{
				value = s.error ? 2 : (s.actual ? 1 : 0);
			}
			else if (ddi == d(DDI::ActualSpacingApplicationRate))
			{
				for (std::size_t p = 0; p < profile.products.size(); p++)
				{
					if (profile.products[p].boom == boom)
					{
						value = model.row_spacing_mm(boom, index, p);
						break;
					}
				}
			}
			else if (ddi == d(DDI::ActualSeedSingulationPercentage) || ddi == d(DDI::AverageSeedSingulationPercentage))
			{
				value = static_cast<std::int32_t>(model.row_singulation(boom, index) * 1000000.0);
			}
			else if (ddi == d(DDI::ActualSeedSkipPercentage) || ddi == d(DDI::AverageSeedSkipPercentage))
			{
				value = static_cast<std::int32_t>((1.0 - model.row_singulation(boom, index)) * 2.0 / 3.0 * 1000000.0);
			}
			else if (ddi == d(DDI::ActualSeedMultiplePercentage) || ddi == d(DDI::AverageSeedMultiplePercentage))
			{
				value = static_cast<std::int32_t>((1.0 - model.row_singulation(boom, index)) / 3.0 * 1000000.0);
			}
		}
		else if (pool.map.bin_for(element, product))
		{
			const auto &p = profile.products[product];
			const auto &s = model.product(product);
			const double rs = rate_scale(p.unit);
			const double cs = content_scale(p.bin.unit);
			switch (ddi)
			{
				case d(DDI::SetpointVolumePerAreaApplicationRate):
				case d(DDI::SetpointMassPerAreaApplicationRate):
				case d(DDI::SetpointCountPerAreaApplicationRate):
					value = to_ddi(model.target_rate(product), rs);
					break;
				case d(DDI::ActualVolumePerAreaApplicationRate):
				case d(DDI::ActualMassPerAreaApplicationRate):
				case d(DDI::ActualCountPerAreaApplicationRate):
					value = to_ddi(s.actualRate, rs);
					break;
				case d(DDI::MaximumVolumeContent):
				case d(DDI::MaximumMassContent):
				case d(DDI::MaximumCountContent):
					value = to_ddi(p.bin.capacity, cs);
					break;
				case d(DDI::ActualVolumeContent):
				case d(DDI::ActualMassContent):
				case d(DDI::ActualCountContent):
					value = to_ddi(s.level, cs);
					break;
				case d(DDI::ApplicationTotalVolume_L):
				case d(DDI::ApplicationTotalMass_kg):
				case d(DDI::ApplicationTotalCount):
					value = to_ddi(s.tripTotal, 1.0);
					break;
				case d(DDI::LifetimeApplicationTotalVolume):
				case d(DDI::LifetimeApplicationTotalMass):
				case d(DDI::LifetimeApplicationTotalCount):
					value = to_ddi(s.lifetimeTotal, 1.0);
					break;
				case d(DDI::PrescriptionControlState):
					value = s.prescription ? 1 : 0;
					break;
				case d(DDI::ActualVolumePerTimeApplicationRate):
					value = to_ddi(s.flowLPerMin, MM3_PER_S_PER_L_PER_MIN);
					break;
				case d(DDI::SetpointProductPressure):
					value = to_ddi(profile.sprayer.nozzlePressure, PA_PER_BAR);
					break;
				case d(DDI::ActualProductPressure):
					value = to_ddi(s.pressureBar, PA_PER_BAR);
					break;
				case d(DDI::SetpointSeedingDepth):
				case d(DDI::ActualSeedingDepth):
					value = model.depth();
					break;
				default:
					break;
			}
		}
		return true;
	}

	void ImplementClient::value_command(std::uint16_t element, std::uint16_t ddi, std::int32_t value)
	{
		nlohmann::json fields = { { "element", element }, { "ddi", ddi }, { "value", value } };
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			std::size_t boom = 0, index = 0, product = 0;
			if (pool.map.boom_for(element, boom))
			{
				fields["boom"] = boom;
				if (ddi == d(DDI::SetpointWorkState))
				{
					model.set_master(boom, value == 1);
					fields["what"] = "master";
				}
				else if (ddi == d(DDI::SectionControlState))
				{
					model.set_auto(boom, value == 1);
					fields["what"] = "auto";
				}
				else if ((ddi >= d(DDI::SetpointCondensedWorkState1_16) && ddi <= d(DDI::SetpointCondensedWorkState241_256)) ||
				         (profile.sectionControl == SectionControl::ActualSettable && ddi >= d(DDI::ActualCondensedWorkState1_16) && ddi <= d(DDI::ActualCondensedWorkState241_256)))
				{
					const std::size_t base = ddi >= d(DDI::SetpointCondensedWorkState1_16) ? d(DDI::SetpointCondensedWorkState1_16) : d(DDI::ActualCondensedWorkState1_16);
					const std::size_t chunk = ddi - base;
					const std::size_t count = model.boom(boom).sections.size();
					for (std::size_t i = 0; i < 16; i++)
					{
						const std::size_t s = chunk * 16 + i;
						if (s >= count)
						{
							break;
						}
						const std::uint32_t bits = (static_cast<std::uint32_t>(value) >> (2 * i)) & 0x03;
						if (bits == 0x03)
						{
							continue; // not installed: no change
						}
						model.set_section_setpoint(boom, s, bits == 0x01);
					}
					fields["what"] = "sections";
					fields["chunk"] = chunk;
				}
				else if (ddi == d(DDI::TotalArea) || ddi == d(DDI::EffectiveTotalDistance) || ddi == d(DDI::IneffectiveTotalDistance) || ddi == d(DDI::EffectiveTotalTime) || ddi == d(DDI::IneffectiveTotalTime))
				{
					if (value == 0)
					{
						model.reset_boom_totals(boom);
					}
					fields["what"] = "totals";
				}
			}
			else if (pool.map.section_for(element, boom, index))
			{
				fields["boom"] = boom;
				fields["section"] = index + 1;
				if (ddi == d(DDI::ActualWorkState) && profile.sectionControl == SectionControl::PerElement)
				{
					model.set_section_setpoint(boom, index, value == 1);
					fields["what"] = "section";
				}
			}
			else if (pool.map.bin_for(element, product))
			{
				const auto &p = profile.products[product];
				fields["product"] = p.name;
				switch (ddi)
				{
					case d(DDI::SetpointVolumePerAreaApplicationRate):
					case d(DDI::SetpointMassPerAreaApplicationRate):
					case d(DDI::SetpointCountPerAreaApplicationRate):
					{
						const double rate = value / rate_scale(p.unit);
						const bool clipped = model.set_tc_rate(product, rate);
						fields["what"] = "rate";
						fields["rate"] = rate;
						fields["clipped"] = clipped;
						break;
					}
					case d(DDI::ActualVolumeContent):
					case d(DDI::ActualMassContent):
					case d(DDI::ActualCountContent):
						model.set_bin_level(product, value / content_scale(p.bin.unit));
						fields["what"] = "content";
						break;
					case d(DDI::ApplicationTotalVolume_L):
					case d(DDI::ApplicationTotalMass_kg):
					case d(DDI::ApplicationTotalCount):
						if (value == 0)
						{
							model.reset_trip_total(product);
						}
						fields["what"] = "tripTotal";
						break;
					case d(DDI::PrescriptionControlState):
						model.set_prescription(product, value == 1);
						fields["what"] = "prescription";
						break;
					case d(DDI::SetpointSeedingDepth):
						model.set_depth(value);
						fields["what"] = "depth";
						break;
					default:
						break;
				}
			}
		}
		events.emit("set", fields);
	}

	void ImplementClient::update()
	{
		std::lock_guard<std::mutex> session(sessionMutex);
		if (!client || !speed)
		{
			return;
		}
		speed->update();

		if (ecu && ecu->get_address_valid() && ecu->get_address() != lastAddress)
		{
			lastAddress = ecu->get_address();
			events.emit("address", { { "address", lastAddress } });
		}
		const int state = static_cast<int>(client->get_state());
		if (state != lastState)
		{
			lastState = state;
			lastStateName = state_name(client->get_state());
			events.emit("tcState", { { "state", lastStateName } });
		}
		const bool connected = client->get_is_connected();
		if (connected != lastConnected)
		{
			lastConnected = connected;
			nlohmann::json fields = { { "connected", connected } };
			if (connected)
			{
				fields["booms"] = client->get_connected_tc_number_booms_supported();
				fields["sections"] = client->get_connected_tc_number_sections_supported();
				fields["channels"] = client->get_connected_tc_number_channels_supported();
				fields["version"] = static_cast<int>(client->get_connected_tc_version());
				fields["address"] = tc->get_address();
			}
			events.emit("tc", fields);
		}
		report_changes();
	}

	void ImplementClient::report_changes()
	{
		std::vector<nlohmann::json> out;
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			for (std::size_t b = 0; b < model.boom_count(); b++)
			{
				const std::string sp = model.setpoint_string(b), cmd = model.commanded_string(b), act = model.actual_string(b);
				if (sp != lastSetpoint[b] || cmd != lastCommanded[b] || act != lastActual[b])
				{
					lastSetpoint[b] = sp;
					lastCommanded[b] = cmd;
					lastActual[b] = act;
					out.push_back({ { "event", "sections" }, { "boom", b }, { "setpoint", sp }, { "commanded", cmd }, { "actual", act } });
				}
				const auto &bs = model.boom(b);
				if (bs.autoMode != lastAuto[b] || bs.master != lastMaster[b])
				{
					lastAuto[b] = bs.autoMode;
					lastMaster[b] = bs.master;
					out.push_back({ { "event", "mode" }, { "boom", b }, { "auto", bs.autoMode }, { "master", bs.master } });
				}
			}
			for (std::size_t p = 0; p < model.product_count(); p++)
			{
				const double target = model.target_rate(p);
				if (target != lastTarget[p])
				{
					lastTarget[p] = target;
					out.push_back({ { "event", "product" }, { "product", profile.products[p].name }, { "target", target }, { "unit", profile.products[p].unit }, { "level", model.product(p).level }, { "prescription", model.product(p).prescription } });
				}
			}
			const bool known = model.speed_known();
			const double kmh = known ? std::round(model.speed_mm_s() * 0.036) / 10.0 : 0.0;
			if (known != lastSpeedKnown || (known && kmh != lastSpeedKmh))
			{
				lastSpeedKnown = known;
				lastSpeedKmh = kmh;
				out.push_back({ { "event", "speed" }, { "known", known }, { "kmh", kmh } });
			}
		}
		for (auto &e : out)
		{
			const std::string name = e["event"];
			e.erase("event");
			events.emit(name, e);
		}
	}
}
