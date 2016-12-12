// Copyright (c) 2016 CloudMakers, s. r. o.
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 Build 0 - PoC by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO LX200 driver
 \file indigo_mount_lx200.c
 */

#define DRIVER_VERSION 0x0001

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <termios.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* could not find these on linux, so I define them as in digi.h - to be tested, at least it compiles now */
#if !defined(TIOCSDTR)
#define TIOCSDTR        ('e'<<8) | 0            /* set DTR              */
#define TIOCCDTR        ('e'<<8) | 1            /* clear DTR            */
#endif

#include "indigo_driver_xml.h"

#include "indigo_mount_lx200.h"

#undef PRIVATE_DATA
#define PRIVATE_DATA        ((lx200_private_data *)DEVICE_CONTEXT->private_data)

#define RA_MIN_DIF					(1/24/60/10)
#define DEC_MIN_DIF					(1/60/60)

typedef struct {
	bool parked;
	int handle;
	int device_count;
	indigo_property *device_port;
	double currentRA;
	double currentDec;
	indigo_timer *position_timer;
	pthread_mutex_t port_mutex;
} lx200_private_data;

bool meade_command(indigo_device *device, char *command, char *response, int max);

bool meade_open(indigo_device *device) {
	struct termios	tty_setting;
	PRIVATE_DATA->handle = open(PRIVATE_DATA->device_port->items->text.value, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (PRIVATE_DATA->handle <= 0) {
		INDIGO_LOG(indigo_log("lx200: failed to connect %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	if (ioctl(PRIVATE_DATA->handle, TIOCEXCL) == -1) {
		INDIGO_LOG(indigo_log("lx200: TIOCEXCL failed on %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	if (fcntl(PRIVATE_DATA->handle, F_SETFL, 0) == -1) {
		INDIGO_LOG(indigo_log("lx200: Clearing O_NONBLOCK failed on %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	if (tcgetattr(PRIVATE_DATA->handle, &tty_setting) == -1) {
		INDIGO_LOG(indigo_log("lx200: Failed to get tty attributes for %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	cfmakeraw(&tty_setting);
	tty_setting.c_cc[VMIN] = 1;
	tty_setting.c_cc[VTIME] = 10;
	cfsetspeed(&tty_setting, B9600);
	tty_setting.c_cflag |= CS8;
	if (tcsetattr(PRIVATE_DATA->handle, TCSANOW, &tty_setting) == -1) {
		INDIGO_LOG(indigo_log("lx200: Failed to set tty attributes for %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	if (ioctl(PRIVATE_DATA->handle, TIOCSDTR) == -1)  {
		INDIGO_LOG(indigo_log("lx200: Failed to assert DTR for %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	if (ioctl(PRIVATE_DATA->handle, TIOCCDTR) == -1) {
		INDIGO_LOG(indigo_log("lx200: Failed to clear DTR for %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	int handshake = TIOCM_DTR | TIOCM_RTS | TIOCM_CTS | TIOCM_DSR;
	if (ioctl(PRIVATE_DATA->handle, TIOCMSET, &handshake) == -1) {
		INDIGO_LOG(indigo_log("lx200: Failed to set handshake lines for %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
		return false;
	}
	INDIGO_LOG(indigo_log("lx200: connected to %s", PRIVATE_DATA->device_port->items->text.value));
	char response[128];
	if (meade_command(device, "\006", response, 1)) {
		INDIGO_LOG(indigo_log("lx200: mode:     %s", response));
	}
	if (meade_command(device, ":GVP#", response, 127)) {
		INDIGO_LOG(indigo_log("lx200: product:  %s", response));
	}
	if (meade_command(device, ":GVN#", response, 127)) {
		INDIGO_LOG(indigo_log("lx200: firmware: %s", response));
	}
	if (meade_command(device, ":GW#", response, 127)) {
		INDIGO_LOG(indigo_log("lx200: status:   %s", response));
	}
	return true;
}

bool meade_command(indigo_device *device, char *command, char *response, int max) {
	pthread_mutex_lock(&PRIVATE_DATA->port_mutex);
	char c;
	struct timeval tv;
	// flush
	while (true) {
		fd_set readout;
		FD_ZERO(&readout);
		FD_SET(PRIVATE_DATA->handle, &readout);
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		long result = select(PRIVATE_DATA->handle+1, &readout, NULL, NULL, &tv);
		if (result == 0)
			break;
		if (result < 0) {
			pthread_mutex_unlock(&PRIVATE_DATA->port_mutex);
			return false;
		}
		result = read(PRIVATE_DATA->handle, &c, 1);
		if (result < 1) {
			pthread_mutex_unlock(&PRIVATE_DATA->port_mutex);
			return false;
		}
	}
	// write command
	unsigned long remains = strlen(command);
	char *data = command;
	long written = 0;
	while (remains > 0) {
		written = write(PRIVATE_DATA->handle, data, remains);
		if (written < 0) {
			INDIGO_LOG(indigo_log("lx200: Failed to write to %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
			pthread_mutex_unlock(&PRIVATE_DATA->port_mutex);
			return false;
		}
		data += written;
		remains -= written;
	}
	// read response
	if (response != NULL) {
		int index = 0;
		remains = max;
		int timeout = 1;
		while (remains > 0) {
			fd_set readout;
			FD_ZERO(&readout);
			FD_SET(PRIVATE_DATA->handle, &readout);
			tv.tv_sec = timeout;
			tv.tv_usec = 100000;
			timeout = 0;
			long result = select(PRIVATE_DATA->handle+1, &readout, NULL, NULL, &tv);
			if (result <= 0)
				break;
			result = read(PRIVATE_DATA->handle, &c, 1);
			if (result < 1) {
				INDIGO_LOG(indigo_log("lx200: Failed to read from %s -> %s (%d)", PRIVATE_DATA->device_port->items->text.value, strerror(errno), errno));
				pthread_mutex_unlock(&PRIVATE_DATA->port_mutex);
				return false;
			}
			if (c < 0)
				c = ':';
			if (c == '#')
				break;
			response[index++] = c;
		}
		response[index] = 0;
	}
	pthread_mutex_unlock(&PRIVATE_DATA->port_mutex);
	INDIGO_LOG(indigo_log("lx200: Command %s -> %s", command, response != NULL ? response : "NULL"));
	return true;
}

void meade_close(indigo_device *device) {
	close(PRIVATE_DATA->handle);
	PRIVATE_DATA->handle = 0;
	INDIGO_LOG(indigo_log("lx200: disconnected from %s", PRIVATE_DATA->device_port->items->text.value));
}

// -------------------------------------------------------------------------------- INDIGO MOUNT device implementation

static void position_timer_callback(indigo_device *device) {
	if (PRIVATE_DATA->handle > 0) {
		char response[128];
		if (meade_command(device, ":GR#", response, 127))
			PRIVATE_DATA->currentRA = indigo_stod(response);
		if (meade_command(device, ":GD#", response, 127))
			PRIVATE_DATA->currentDec = indigo_stod(response);
		double diffRA = MOUNT_EQUATORIAL_COORDINATES_RA_ITEM->number.target - PRIVATE_DATA->currentRA;
		double diffDec = MOUNT_EQUATORIAL_COORDINATES_DEC_ITEM->number.target - PRIVATE_DATA->currentDec;
		if (diffRA <= RA_MIN_DIF && diffDec <= DEC_MIN_DIF)
			MOUNT_EQUATORIAL_COORDINATES_PROPERTY->state = INDIGO_OK_STATE;
		else
			MOUNT_EQUATORIAL_COORDINATES_PROPERTY->state = INDIGO_BUSY_STATE;
		MOUNT_EQUATORIAL_COORDINATES_RA_ITEM->number.value = PRIVATE_DATA->currentRA;
		MOUNT_EQUATORIAL_COORDINATES_DEC_ITEM->number.value = PRIVATE_DATA->currentDec;
		indigo_update_property(device, MOUNT_EQUATORIAL_COORDINATES_PROPERTY, NULL);
		PRIVATE_DATA->position_timer = indigo_set_timer(device, 0.2, position_timer_callback);
	}
}

static indigo_result mount_attach(indigo_device *device) {
	assert(device != NULL);
	assert(device->device_context != NULL);
	lx200_private_data *private_data = device->device_context;
	device->device_context = NULL;
	if (indigo_mount_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		DEVICE_CONTEXT->private_data = private_data;
		// -------------------------------------------------------------------------------- SIMULATION
		SIMULATION_PROPERTY->hidden = true;
		// -------------------------------------------------------------------------------- MOUNT_PARK
		MOUNT_PARK_PROPERTY->count = 1;
		MOUNT_PARK_PARKED_ITEM->sw.value = false;
		// -------------------------------------------------------------------------------- MOUNT_ON_COORDINATES_SET
		MOUNT_ON_COORDINATES_SET_PROPERTY->count = 2;
		// -------------------------------------------------------------------------------- DEVICE_PORT
		DEVICE_PORT_PROPERTY->hidden = false;
		PRIVATE_DATA->device_port = DEVICE_PORT_PROPERTY;
		// -------------------------------------------------------------------------------- DEVICE_PORTS
		DEVICE_PORTS_PROPERTY->hidden = false;
		// --------------------------------------------------------------------------------
		pthread_mutex_init(&PRIVATE_DATA->port_mutex, NULL);
		INDIGO_LOG(indigo_log("%s attached", device->name));
		return indigo_mount_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result mount_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(device->device_context != NULL);
	assert(property != NULL);
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			bool result = true;
			if (PRIVATE_DATA->device_count++ == 0) {
				CONNECTION_PROPERTY->state = INDIGO_BUSY_STATE;
				indigo_update_property(device, CONNECTION_PROPERTY, NULL);
				result = meade_open(device);
				position_timer_callback(device);
			}
			if (result) {
				CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
			} else {
				PRIVATE_DATA->device_count--;
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
			}
		} else {
			if (PRIVATE_DATA->position_timer != NULL)
				indigo_cancel_timer(device, PRIVATE_DATA->position_timer);
			PRIVATE_DATA->position_timer = NULL;
			if (--PRIVATE_DATA->device_count == 0) {
				meade_close(device);
			}
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
	} else if (indigo_property_match(MOUNT_PARK_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- MOUNT_PARK
		indigo_property_copy_values(MOUNT_PARK_PROPERTY, property, false);
		if (MOUNT_PARK_PARKED_ITEM->sw.value) {
			meade_command(device, ":hP#", NULL, 0);
			PRIVATE_DATA->parked = true;
			indigo_update_property(device, MOUNT_PARK_PROPERTY, NULL);
		}
		return INDIGO_OK;
	} else if (indigo_property_match(MOUNT_EQUATORIAL_COORDINATES_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- MOUNT_EQUATORIAL_COORDINATES
		indigo_property_copy_values(MOUNT_EQUATORIAL_COORDINATES_PROPERTY, property, false);
		if (MOUNT_ON_COORDINATES_SET_TRACK_ITEM->sw.value) {
			MOUNT_EQUATORIAL_COORDINATES_PROPERTY->state = INDIGO_OK_STATE;
			char command[128], response[128];
			sprintf(command, ":Sr%s#", indigo_dtos(MOUNT_EQUATORIAL_COORDINATES_RA_ITEM->number.value, "%02d:%02d:%02.0f"));
			if (!meade_command(device, command, response, 1) || *response != '1') {
				INDIGO_LOG(indigo_log("lx200: %s failed", command));
				MOUNT_EQUATORIAL_COORDINATES_PROPERTY->state = INDIGO_ALERT_STATE;
			} else {
				sprintf(command, ":Sd%s#", indigo_dtos(MOUNT_EQUATORIAL_COORDINATES_DEC_ITEM->number.value, "%+03d*%02d:%02.0f"));
				if (!meade_command(device, command, response, 1) || *response != '1') {
					INDIGO_LOG(indigo_log("lx200: %s failed", command));
					MOUNT_EQUATORIAL_COORDINATES_PROPERTY->state = INDIGO_ALERT_STATE;
				} else if (!meade_command(device, ":MS#", response, 1) || *response != '0') {
					INDIGO_LOG(indigo_log("lx200: :MS# failed"));
					MOUNT_EQUATORIAL_COORDINATES_PROPERTY->state = INDIGO_ALERT_STATE;
				}
			}
		}
		indigo_update_property(device, MOUNT_EQUATORIAL_COORDINATES_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(MOUNT_ABORT_MOTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- MOUNT_ABORT_MOTION
		indigo_property_copy_values(MOUNT_ABORT_MOTION_PROPERTY, property, false);
		if (PRIVATE_DATA->position_timer != NULL) {
			indigo_cancel_timer(device, PRIVATE_DATA->position_timer);
			PRIVATE_DATA->position_timer = NULL;
			meade_command(device, ":Q#", NULL, 0);
			MOUNT_EQUATORIAL_COORDINATES_PROPERTY->state = INDIGO_ALERT_STATE;
			indigo_update_property(device, MOUNT_EQUATORIAL_COORDINATES_PROPERTY, NULL);
		}
		MOUNT_ABORT_MOTION_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, MOUNT_ABORT_MOTION_PROPERTY, "Aborted");
		return INDIGO_OK;
		// --------------------------------------------------------------------------------
	}
	return indigo_mount_change_property(device, client, property);
}

static indigo_result mount_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device);
	INDIGO_LOG(indigo_log("%s detached", device->name));
	return indigo_mount_detach(device);
}

// -------------------------------------------------------------------------------- INDIGO guider device implementation

static indigo_result guider_attach(indigo_device *device) {
	assert(device != NULL);
	assert(device->device_context != NULL);
	lx200_private_data *private_data = device->device_context;
	device->device_context = NULL;
	if (indigo_guider_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		DEVICE_CONTEXT->private_data = private_data;
		INDIGO_LOG(indigo_log("%s attached", device->name));
		return indigo_guider_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result guider_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(device->device_context != NULL);
	assert(property != NULL);
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			bool result = true;
			if (PRIVATE_DATA->device_count++ == 0) {
				CONNECTION_PROPERTY->state = INDIGO_BUSY_STATE;
				indigo_update_property(device, CONNECTION_PROPERTY, NULL);
				result = meade_open(device);
			}
			if (result) {
				CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
			} else {
				PRIVATE_DATA->device_count--;
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
			}
		} else {
			if (--PRIVATE_DATA->device_count == 0) {
				meade_close(device);
			}
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
	} else if (indigo_property_match(GUIDER_GUIDE_DEC_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- GUIDER_GUIDE_DEC
		indigo_property_copy_values(GUIDER_GUIDE_DEC_PROPERTY, property, false);
		
		// TODO
		
		GUIDER_GUIDE_DEC_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, GUIDER_GUIDE_DEC_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(GUIDER_GUIDE_RA_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- GUIDER_GUIDE_RA
		indigo_property_copy_values(GUIDER_GUIDE_RA_PROPERTY, property, false);
		
		// TODO
		
		GUIDER_GUIDE_RA_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, GUIDER_GUIDE_RA_PROPERTY, NULL);
		return INDIGO_OK;
		// --------------------------------------------------------------------------------
	}
	return indigo_guider_change_property(device, client, property);
}

static indigo_result guider_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device);
	INDIGO_LOG(indigo_log("%s detached", device->name));
	return indigo_guider_detach(device);
}

// --------------------------------------------------------------------------------

static lx200_private_data *private_data = NULL;

static indigo_device *mount = NULL;
static indigo_device *mount_guider = NULL;

indigo_result indigo_mount_lx200(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_device mount_template = {
		MOUNT_LX200_NAME, NULL, INDIGO_OK, INDIGO_VERSION_CURRENT,
		mount_attach,
		indigo_mount_enumerate_properties,
		mount_change_property,
		mount_detach
	};
	static indigo_device mount_guider_template = {
		MOUNT_LX200_GUIDER_NAME, NULL, INDIGO_OK, INDIGO_VERSION_CURRENT,
		guider_attach,
		indigo_guider_enumerate_properties,
		guider_change_property,
		guider_detach
	};
	
	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;
	
	SET_DRIVER_INFO(info, MOUNT_LX200_NAME, __FUNCTION__, DRIVER_VERSION, last_action);
	
	if (action == last_action)
		return INDIGO_OK;
	
	switch (action) {
		case INDIGO_DRIVER_INIT:
			last_action = action;
			private_data = malloc(sizeof(lx200_private_data));
			memset(private_data, 0, sizeof(lx200_private_data));
			assert(private_data != NULL);
			mount = malloc(sizeof(indigo_device));
			assert(mount != NULL);
			memcpy(mount, &mount_template, sizeof(indigo_device));
			mount->device_context = private_data;
			indigo_attach_device(mount);
			mount_guider = malloc(sizeof(indigo_device));
			assert(mount_guider != NULL);
			memcpy(mount_guider, &mount_guider_template, sizeof(indigo_device));
			mount_guider->device_context = private_data;
			indigo_attach_device(mount_guider);
			break;
			
		case INDIGO_DRIVER_SHUTDOWN:
			last_action = action;
			if (mount != NULL) {
				indigo_detach_device(mount);
				free(mount);
				mount = NULL;
			}
			if (mount_guider != NULL) {
				indigo_detach_device(mount_guider);
				free(mount_guider);
				mount_guider = NULL;
			}
			if (private_data != NULL) {
				free(private_data);
				private_data = NULL;
			}
			break;
			
		case INDIGO_DRIVER_INFO:
			break;
	}
	
	return INDIGO_OK;
}
