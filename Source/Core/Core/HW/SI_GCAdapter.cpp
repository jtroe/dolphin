// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <libusb.h>

#include "Core/ConfigManager.h"
#include "Core/HW/SI_GCAdapter.h"

namespace SI_GCAdapter
{
enum ControllerTypes
{
	CONTROLLER_NONE = 0,
	CONTROLLER_WIRED = 1,
	CONTROLLER_WIRELESS = 2
};

static libusb_device_handle* s_handle = nullptr;
static u8 s_controller_type[MAX_SI_CHANNELS] = { CONTROLLER_NONE, CONTROLLER_NONE, CONTROLLER_NONE, CONTROLLER_NONE };
static u8 s_controller_rumble[4];

static std::mutex s_mutex;
static u8 s_controller_payload[37];
static int s_controller_payload_size = 0;

static std::thread s_adapter_thread;
static Common::Flag s_adapter_thread_running;

static bool s_libusb_driver_not_supported = false;

static u8 s_endpoint_in = 0;
static u8 s_endpoint_out = 0;

static void Read()
{
	while (s_adapter_thread_running.IsSet())
	{
		u8 controller_payload_swap[37];

		libusb_interrupt_transfer(s_handle, s_endpoint_in, s_controller_payload, sizeof(controller_payload_swap), &s_controller_payload_size, 0);

		{
		std::lock_guard<std::mutex> lk(s_mutex);
		std::swap(controller_payload_swap, s_controller_payload);
		}

		Common::YieldCPU();
	}
}

void Init()
{
	if (s_handle != nullptr)
		return;

	s_libusb_driver_not_supported = false;

	for (int i = 0; i < MAX_SI_CHANNELS; i++)
	{
		s_controller_type[i] = CONTROLLER_NONE;
		s_controller_rumble[i] = 0;
	}

	int ret = libusb_init(nullptr);

	if (ret)
	{
		ERROR_LOG(SERIALINTERFACE, "libusb_init failed with error: %d", ret);
		Shutdown();
	}
	else
	{
		libusb_device** list;
		ssize_t cnt = libusb_get_device_list(nullptr, &list);
		for (int d = 0; d < cnt; d++)
		{
			libusb_device* device = list[d];
			libusb_device_descriptor desc;
			int dRet = libusb_get_device_descriptor(device, &desc);
			if (dRet)
			{
				// could not aquire the descriptor, no point in trying to use it.
				ERROR_LOG(SERIALINTERFACE, "libusb_get_device_descriptor failed with error: %d", dRet);
				continue;
			}

			if (desc.idVendor == 0x057e && desc.idProduct == 0x0337)
			{
				NOTICE_LOG(SERIALINTERFACE, "Found GC Adapter with Vendor: %X Product: %X Devnum: %d", desc.idVendor, desc.idProduct, 1);

				u8 bus = libusb_get_bus_number(device);
				u8 port = libusb_get_device_address(device);
				ret = libusb_open(device, &s_handle);
				if (ret)
				{
					if (ret == LIBUSB_ERROR_ACCESS)
					{
						if (dRet)
						{
							ERROR_LOG(SERIALINTERFACE, "Dolphin does not have access to this device: Bus %03d Device %03d: ID ????:???? (couldn't get id).",
								bus,
								port
								);
						}
						else
						{
							ERROR_LOG(SERIALINTERFACE, "Dolphin does not have access to this device: Bus %03d Device %03d: ID %04X:%04X.",
								bus,
								port,
								desc.idVendor,
								desc.idProduct
								);
						}
					}
					else
					{
						ERROR_LOG(SERIALINTERFACE, "libusb_open failed to open device with error = %d", ret);
						if (ret == LIBUSB_ERROR_NOT_SUPPORTED)
							s_libusb_driver_not_supported = true;
					}
					Shutdown();
				}
				else if ((ret = libusb_kernel_driver_active(s_handle, 0)) == 1)
				{
					if ((ret = libusb_detach_kernel_driver(s_handle, 0)) && ret != LIBUSB_ERROR_NOT_SUPPORTED)
					{
						ERROR_LOG(SERIALINTERFACE, "libusb_detach_kernel_driver failed with error: %d", ret);
						Shutdown();
					}
				}
				else if (ret != 0 && ret != LIBUSB_ERROR_NOT_SUPPORTED)
				{
					ERROR_LOG(SERIALINTERFACE, "libusb_kernel_driver_active error ret = %d", ret);
					Shutdown();
				}
				else if ((ret = libusb_claim_interface(s_handle, 0)))
				{
					ERROR_LOG(SERIALINTERFACE, "libusb_claim_interface failed with error: %d", ret);
					Shutdown();
				}
				else
				{
					libusb_config_descriptor *config = nullptr;
					libusb_get_config_descriptor(device, 0, &config);
					for (u8 ic = 0; ic < config->bNumInterfaces; ic++)
					{
						const libusb_interface *interfaceContainer = &config->interface[ic];
						for (int i = 0; i < interfaceContainer->num_altsetting; i++)
						{
							const libusb_interface_descriptor *interface = &interfaceContainer->altsetting[i];
							for (int e = 0; e < (int)interface->bNumEndpoints; e++)
							{
								const libusb_endpoint_descriptor *endpoint = &interface->endpoint[e];
								if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN)
									s_endpoint_in = endpoint->bEndpointAddress;
								else
									s_endpoint_out = endpoint->bEndpointAddress;
							}
						}
					}

					int tmp = 0;
					unsigned char payload = 0x13;
					libusb_interrupt_transfer(s_handle, s_endpoint_out, &payload, sizeof(payload), &tmp, 0);

					RefreshConnectedDevices();

					s_adapter_thread_running.Set(true);
					s_adapter_thread = std::thread(Read);
				}
			}
		}

		libusb_free_device_list(list, 1);
	}
}

void Shutdown()
{
	if (s_handle == nullptr || !SConfig::GetInstance().m_GameCubeAdapter)
		return;

	if (s_adapter_thread_running.TestAndClear())
	{
		s_adapter_thread.join();
	}

	libusb_close(s_handle);
	s_libusb_driver_not_supported = false;

	for (int i = 0; i < MAX_SI_CHANNELS; i++)
		s_controller_type[i] = CONTROLLER_NONE;

	s_handle = nullptr;
}

void Input(int chan, GCPadStatus* pad)
{
	if (s_handle == nullptr || !SConfig::GetInstance().m_GameCubeAdapter)
		return;

	u8 controller_payload_copy[37];

	{
	std::lock_guard<std::mutex> lk(s_mutex);
	std::copy(std::begin(s_controller_payload), std::end(s_controller_payload), std::begin(controller_payload_copy));
	}

	if (s_controller_payload_size != sizeof(controller_payload_copy) || controller_payload_copy[0] != LIBUSB_DT_HID)
	{
		ERROR_LOG(SERIALINTERFACE, "error reading payload (size: %d, type: %02x)", s_controller_payload_size, controller_payload_copy[0]);
	}
	else
	{
		u8 type = controller_payload_copy[1 + (9 * chan)] >> 4;
		if (type != CONTROLLER_NONE && s_controller_type[chan] == CONTROLLER_NONE)
			NOTICE_LOG(SERIALINTERFACE, "New device connected to Port %d of Type: %02x", chan + 1, controller_payload_copy[1 + (9 * chan)]);

		s_controller_type[chan] = type;

		if (s_controller_type[chan] != CONTROLLER_NONE)
		{
			u8 b1 = controller_payload_copy[1 + (9 * chan) + 1];
			u8 b2 = controller_payload_copy[1 + (9 * chan) + 2];

			if (b1 & (1 << 0)) pad->button |= PAD_BUTTON_A;
			if (b1 & (1 << 1)) pad->button |= PAD_BUTTON_B;
			if (b1 & (1 << 2)) pad->button |= PAD_BUTTON_X;
			if (b1 & (1 << 3)) pad->button |= PAD_BUTTON_Y;

			if (b1 & (1 << 4)) pad->button |= PAD_BUTTON_LEFT;
			if (b1 & (1 << 5)) pad->button |= PAD_BUTTON_RIGHT;
			if (b1 & (1 << 6)) pad->button |= PAD_BUTTON_DOWN;
			if (b1 & (1 << 7)) pad->button |= PAD_BUTTON_UP;

			if (b2 & (1 << 0)) pad->button |= PAD_BUTTON_START;
			if (b2 & (1 << 1)) pad->button |= PAD_TRIGGER_Z;
			if (b2 & (1 << 2)) pad->button |= PAD_TRIGGER_R;
			if (b2 & (1 << 3)) pad->button |= PAD_TRIGGER_L;

			pad->stickX = controller_payload_copy[1 + (9 * chan) + 3];
			pad->stickY = controller_payload_copy[1 + (9 * chan) + 4];
			pad->substickX = controller_payload_copy[1 + (9 * chan) + 5];
			pad->substickY = controller_payload_copy[1 + (9 * chan) + 6];
			pad->triggerLeft = controller_payload_copy[1 + (9 * chan) + 7];
			pad->triggerRight = controller_payload_copy[1 + (9 * chan) + 8];
		}
	}
}

void Output(int chan, u8 rumble_command)
{
	if (s_handle == nullptr || !SConfig::GetInstance().m_GameCubeAdapter)
		return;

	// Skip over rumble commands if it has not changed or the controller is wireless
	if (rumble_command != s_controller_rumble[chan] && s_controller_type[chan] != CONTROLLER_WIRELESS)
	{
		s_controller_rumble[chan] = rumble_command;

		unsigned char rumble[5] = { 0x11, s_controller_rumble[0], s_controller_rumble[1], s_controller_rumble[2], s_controller_rumble[3] };
		int size = 0;
		libusb_interrupt_transfer(s_handle, s_endpoint_out, rumble, sizeof(rumble), &size, 0);

		if (size != 0x05)
		{
			WARN_LOG(SERIALINTERFACE, "error writing rumble (size: %d)", size);
			Shutdown();
		}
	}
}

SIDevices GetDeviceType(int channel)
{
	if (s_handle == nullptr || !SConfig::GetInstance().m_GameCubeAdapter)
		return SIDEVICE_NONE;

	switch (s_controller_type[channel])
	{
	case CONTROLLER_WIRED:
		return SIDEVICE_GC_CONTROLLER;
	case CONTROLLER_WIRELESS:
		return SIDEVICE_GC_CONTROLLER;
	default:
		return SIDEVICE_NONE;
	}
}

void RefreshConnectedDevices()
{
	if (s_handle == nullptr || !SConfig::GetInstance().m_GameCubeAdapter)
		return;

	int size = 0;
	u8 refresh_controller_payload[37];

	libusb_interrupt_transfer(s_handle, s_endpoint_in, refresh_controller_payload, sizeof(refresh_controller_payload), &size, 0);

	if (size != sizeof(refresh_controller_payload) || refresh_controller_payload[0] != LIBUSB_DT_HID)
	{
		WARN_LOG(SERIALINTERFACE, "error reading payload (size: %d)", size);
		Shutdown();
	}
	else
	{
		for (int chan = 0; chan < MAX_SI_CHANNELS; chan++)
		{
			u8 type = refresh_controller_payload[1 + (9 * chan)] >> 4;
			if (type != CONTROLLER_NONE && s_controller_type[chan] == CONTROLLER_NONE)
				NOTICE_LOG(SERIALINTERFACE, "New device connected to Port %d of Type: %02x", chan + 1, refresh_controller_payload[1 + (9 * chan)]);

			s_controller_type[chan] = type;
		}
	}
}

bool IsDetected()
{
	return s_handle != nullptr;
}

bool IsDriverDetected()
{
	return !s_libusb_driver_not_supported;
}

} // end of namespace SI_GCAdapter
