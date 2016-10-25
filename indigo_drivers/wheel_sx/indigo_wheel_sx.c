//  Copyright (c) 2016 CloudMakers, s. r. o.
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions
//  are met:
//
//  1. Redistributions of source code must retain the above copyright
//  notice, this list of conditions and the following disclaimer.
//
//  2. Redistributions in binary form must reproduce the above
//  copyright notice, this list of conditions and the following
//  disclaimer in the documentation and/or other materials provided
//  with the distribution.
//
//  3. The name of the author may not be used to endorse or promote
//  products derived from this software without specific prior
//  written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE AUTHOR 'AS IS' AND ANY EXPRESS
//  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
//  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
//  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
//  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

//  version history
//  2.0 Build 0 - PoC by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO StarlighXpress filter wheel driver
 \file indigo_ccd_sx.c
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

#if defined(INDIGO_FREEBSD)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <hidapi/hidapi.h>

#include "indigo_driver_xml.h"

#include "indigo_wheel_sx.h"

// -------------------------------------------------------------------------------- SX USB interface implementation

#define SX_VENDOR_ID                  0x1278
#define SX_PRODUC_ID                  0x0920

#undef PRIVATE_DATA
#define PRIVATE_DATA        ((sx_private_data *)DEVICE_CONTEXT->private_data)

#undef INDIGO_DEBUG
#define INDIGO_DEBUG(c) c

typedef struct {
	hid_device *handle;
	int current_slot, target_slot;
	int count;
} sx_private_data;

static bool sx_message(indigo_device *device, int a, int b) {
	unsigned char buf[2] = { a, b };
	int rc = hid_write(PRIVATE_DATA->handle, buf, 2);
	INDIGO_DEBUG(indigo_debug("sx_message: hid_write( { %02x, %02x }) [%d] ->  %d", buf[0], buf[1], __LINE__, rc));
	if (rc != 2)
		return false;
	usleep(100);
	rc = hid_read(PRIVATE_DATA->handle, buf, 2);
	INDIGO_DEBUG(indigo_debug("sx_message: hid_read() [%d] ->  %d, { %02x, %02x }", __LINE__, rc, buf[0], buf[1]));
	PRIVATE_DATA->current_slot = buf[0];
	PRIVATE_DATA->count = buf[1];
	return rc == 2;
}

static bool sx_open(indigo_device *device) {
	if ((PRIVATE_DATA->handle = hid_open(SX_VENDOR_ID, SX_PRODUC_ID, NULL)) != NULL) {
		INDIGO_DEBUG(indigo_debug("sx_open: hid_open [%d] ->  ok", __LINE__));
		sx_message(device, 0x81, 0);
		return true;
	}
	INDIGO_DEBUG(indigo_debug("sx_open: hid_open [%d] ->  failed", __LINE__));
	return false;
}

static void sx_close(indigo_device *device) {
	hid_close(PRIVATE_DATA->handle);
}

// -------------------------------------------------------------------------------- INDIGO CCD device implementation

static void wheel_timer_callback(indigo_device *device) {
	sx_message(device, 0, 0);
	WHEEL_SLOT_ITEM->number.value = PRIVATE_DATA->current_slot;
	if (PRIVATE_DATA->current_slot == PRIVATE_DATA->target_slot) {
		WHEEL_SLOT_PROPERTY->state = INDIGO_OK_STATE;
	} else {
		indigo_set_timer(device, 0.5, wheel_timer_callback);
	}
	indigo_update_property(device, WHEEL_SLOT_PROPERTY, NULL);
}

static indigo_result wheel_attach(indigo_device *device) {
	assert(device != NULL);
	assert(device->device_context != NULL);
	sx_private_data *private_data = device->device_context;
	device->device_context = NULL;
	if (indigo_wheel_device_attach(device, INDIGO_VERSION_CURRENT) == INDIGO_OK) {
		DEVICE_CONTEXT->private_data = private_data;
		INDIGO_LOG(indigo_log("%s attached", device->name));
		return indigo_wheel_device_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result wheel_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(device->device_context != NULL);
	assert(property != NULL);
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);

		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			if (sx_open(device)) {
				WHEEL_SLOT_ITEM->number.max = WHEEL_SLOT_NAME_PROPERTY->count = PRIVATE_DATA->count;
				PRIVATE_DATA->target_slot = 1;
				CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
				indigo_set_timer(device, 0.5, wheel_timer_callback);
			} else {
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
			}
		} else {
			sx_close(device);
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
		
		CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
	} else if (indigo_property_match(WHEEL_SLOT_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- WHEEL_SLOT
		indigo_property_copy_values(WHEEL_SLOT_PROPERTY, property, false);
		if (WHEEL_SLOT_ITEM->number.value < 1 || WHEEL_SLOT_ITEM->number.value > WHEEL_SLOT_ITEM->number.max) {
			WHEEL_SLOT_PROPERTY->state = INDIGO_ALERT_STATE;
		} else if (WHEEL_SLOT_ITEM->number.value == PRIVATE_DATA->current_slot) {
			WHEEL_SLOT_PROPERTY->state = INDIGO_OK_STATE;
		} else {
			WHEEL_SLOT_PROPERTY->state = INDIGO_BUSY_STATE;
			PRIVATE_DATA->target_slot = WHEEL_SLOT_ITEM->number.value;
			WHEEL_SLOT_ITEM->number.value = PRIVATE_DATA->current_slot;
			sx_message(device, 0x80 + PRIVATE_DATA->target_slot, 0);
			indigo_set_timer(device, 0.5, wheel_timer_callback);
		}
		indigo_update_property(device, WHEEL_SLOT_PROPERTY, NULL);
		return INDIGO_OK;
		// --------------------------------------------------------------------------------
	}
	return indigo_wheel_device_change_property(device, client, property);
}

static indigo_result wheel_detach(indigo_device *device) {
	assert(device != NULL);
	INDIGO_LOG(indigo_log("%s detached", device->name));
	return indigo_wheel_device_detach(device);
}

// -------------------------------------------------------------------------------- hot-plug support

static indigo_device *device = NULL;

static int sx_hotplug_callback(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data) {
	static indigo_device wheel_template = {
		"SX Filter Wheel", NULL, INDIGO_OK, INDIGO_VERSION_CURRENT,
		wheel_attach,
		indigo_wheel_device_enumerate_properties,
		wheel_change_property,
		wheel_detach
	};
	switch (event) {
		case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED: {
			if (device != NULL)
				return 0;
			device = malloc(sizeof(indigo_device));
			assert(device != NULL);
			memcpy(device, &wheel_template, sizeof(indigo_device));
			device->device_context = malloc(sizeof(sx_private_data));
			assert(device->device_context);
			memset(device->device_context, 0, sizeof(sx_private_data));
			indigo_attach_device(device);
			break;
		}
		case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT: {
			if (device == NULL)
				return 0;
			indigo_detach_device(device);
			free(device->device_context);
			free(device);
			device = NULL;
		}
	}
	return 0;
};

indigo_result indigo_wheel_sx() {
	libusb_init(NULL);
	hid_init();
	int rc = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, LIBUSB_HOTPLUG_ENUMERATE, SX_VENDOR_ID, SX_PRODUC_ID, LIBUSB_HOTPLUG_MATCH_ANY, sx_hotplug_callback, NULL, NULL);
	INDIGO_DEBUG(indigo_debug("indigo_ccd_sx: libusb_hotplug_register_callback [%d] ->  %s", __LINE__, libusb_error_name(rc)));
	indigo_start_usb_even_handler();
	return rc >= 0;
}

