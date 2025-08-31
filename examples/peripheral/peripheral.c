/*
 *   Copyright (c) 2022 Martijn van Welie
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 */

#include <glib.h>
#include <stdio.h>
#include <signal.h>
#include "adapter.h"
#include "device.h"
#include "logger.h"
#include "agent.h"
#include "application.h"
#include "advertisement.h"
#include "utility.h"
#include "parser.h"

#define TAG "Main"
#define HTS_SERVICE_UUID "00001809-0000-1000-8000-00805f9b34fb"
#define TEMPERATURE_CHAR_UUID "00002a1c-0000-1000-8000-00805f9b34fb"
#define CUD_CHAR "00002901-0000-1000-8000-00805f9b34fb"

GMainLoop *loop = NULL;
Adapter *default_adapter = NULL;
Advertisement *advertisement = NULL;
Application *app = NULL;
Agent *agent = NULL;


void on_powered_state_changed(Adapter *adapter, gboolean state) {
    log_debug(TAG, "powered '%s' (%s)", state ? "on" : "off", binc_adapter_get_path(adapter));
}

void on_central_state_changed(Adapter *adapter, Device *device) {
    char *deviceToString = binc_device_to_string(device);
    log_debug(TAG, deviceToString);
    g_free(deviceToString);

    log_debug(TAG, "remote central %s is %s", binc_device_get_address(device), binc_device_get_connection_state_name(device));
    ConnectionState state = binc_device_get_connection_state(device);
    if (state == BINC_CONNECTED) {
        binc_adapter_stop_advertising(adapter, advertisement);
    } else if (state == BINC_DISCONNECTED){
        binc_adapter_start_advertising(adapter, advertisement);
    }
}

// This function is called when a read is done
// Use this to set the characteristic value if it is not set or to reject the read request
const char *on_local_char_read(const Application *application, const char *address, const char *service_uuid,
                        const char *char_uuid, const guint16 mtu, const guint16 offset) {
    if (g_str_equal(service_uuid, HTS_SERVICE_UUID) && g_str_equal(char_uuid, TEMPERATURE_CHAR_UUID)) {
        const guint8 bytes[] = {0x06, 0x6f, 0x01, 0x00, 0xff, 0xe6, 0x07, 0x03, 0x03, 0x10, 0x04, 0x00, 0x01};
        GByteArray *byteArray = g_byte_array_sized_new(sizeof(bytes));
        g_byte_array_append(byteArray, bytes, sizeof(bytes));
        binc_application_set_char_value(application, service_uuid, char_uuid, byteArray);
        g_byte_array_free(byteArray, TRUE);
        return NULL;
    }
    return BLUEZ_ERROR_REJECTED;
}

// This function should be used to validate or reject a write request
const char *on_local_char_write(const Application *application, const char *address, const char *service_uuid,
                          const char *char_uuid, GByteArray *byteArray, const guint16 mtu, const guint16 offset) {
    GString *result = g_byte_array_as_hex(byteArray);
    log_debug(TAG, "write request characteristic <%s> with value <%s>", char_uuid, result->str);
    g_string_free(result, TRUE);

    return NULL;
}

// This function is called after a write request was validates and the characteristic value was set
void on_local_char_updated(const Application *application, const char *service_uuid,
                           const char *char_uuid, GByteArray *byteArray) {
    GString *result = g_byte_array_as_hex(byteArray);
    log_debug(TAG, "characteristic <%s> updated to <%s>", char_uuid, result->str);
    g_string_free(result, TRUE);
}

void on_local_char_start_notify(const Application *application, const char *service_uuid, const char *char_uuid) {
    log_debug(TAG, "on start notify");
    if (g_str_equal(service_uuid, HTS_SERVICE_UUID) && g_str_equal(char_uuid, TEMPERATURE_CHAR_UUID)) {
        const guint8 bytes[] = {0x06, 0x6A, 0x01, 0x00, 0xff, 0xe6, 0x07, 0x03, 0x03, 0x10, 0x04, 0x00, 0x01};
        GByteArray *byteArray = g_byte_array_sized_new(sizeof(bytes));
        g_byte_array_append(byteArray, bytes, sizeof(bytes));
        binc_application_notify(application, service_uuid, char_uuid, byteArray);
        g_byte_array_free(byteArray, TRUE);
    }
}

void on_local_char_stop_notify(const Application *application, const char *service_uuid, const char *char_uuid) {
    log_debug(TAG, "on stop notify");
}

gboolean on_request_authorization(Device *device) {
    log_debug(TAG, "requesting authorization for '%s", binc_device_get_name(device));
    return TRUE;
}

guint32 on_request_passkey(Device *device) {
    guint32 pass = 000000;
    log_debug(TAG, "requesting passkey for '%s", binc_device_get_name(device));
    log_debug(TAG, "Enter 6 digit pin code: ");
    int result = fscanf(stdin, "%d", &pass);
    if (result != 1) {
        log_debug(TAG, "didn't read a pin code");
    }
    return pass;
}

gboolean callback(gpointer data) {
    if (agent != NULL) {
        binc_agent_free(agent);
        agent = NULL;
    }

    if (app != NULL) {
        binc_adapter_unregister_application(default_adapter, app);
        binc_application_free(app);
        app = NULL;
    }

    if (advertisement != NULL) {
        binc_adapter_stop_advertising(default_adapter, advertisement);
        binc_advertisement_free(advertisement);
    }

    if (default_adapter != NULL) {
        binc_adapter_free(default_adapter);
        default_adapter = NULL;
    }

    g_main_loop_quit((GMainLoop *) data);
    return FALSE;
}

static void cleanup_handler(int signo) {
    if (signo == SIGINT) {
        log_error(TAG, "received SIGINT");
        callback(loop);
    }
}

int main(void) {
    // Get a DBus connection
    GDBusConnection *dbusConnection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

    // Setup handler for CTRL+C
    if (signal(SIGINT, cleanup_handler) == SIG_ERR)
        log_error(TAG, "can't catch SIGINT");

    // Setup mainloop
    loop = g_main_loop_new(NULL, FALSE);

    // Get the default default_adapter
    default_adapter = binc_adapter_get_default(dbusConnection);

    if (default_adapter != NULL) {
        log_debug(TAG, "using default_adapter '%s'", binc_adapter_get_path(default_adapter));

        // Make sure the adapter is on
        binc_adapter_set_powered_state_cb(default_adapter, &on_powered_state_changed);
        if (!binc_adapter_get_powered_state(default_adapter)) {
            binc_adapter_power_on(default_adapter);
        }

        // Register an agent and set callbacks
        agent = binc_agent_create(default_adapter, "/org/bluez/BincAgent", KEYBOARD_DISPLAY);
        binc_agent_set_request_authorization_cb(agent, &on_request_authorization);
        binc_agent_set_request_passkey_cb(agent, &on_request_passkey);

        // Setup remote central connection state callback
        binc_adapter_set_remote_central_cb(default_adapter, &on_central_state_changed);

        // Setup advertisement
        GPtrArray *adv_service_uuids = g_ptr_array_new();
        g_ptr_array_add(adv_service_uuids, HTS_SERVICE_UUID);

        advertisement = binc_advertisement_create();
        binc_advertisement_set_local_name(advertisement, "BINC");
        binc_advertisement_set_secondary_channel(advertisement, BINC_SC_2M);
        binc_advertisement_set_interval(advertisement, 500, 500);
        binc_advertisement_set_tx_power(advertisement, 5);
        binc_advertisement_set_services(advertisement, adv_service_uuids);
        g_ptr_array_free(adv_service_uuids, TRUE);
        binc_adapter_start_advertising(default_adapter, advertisement);

        // Start application
        app = binc_create_application(default_adapter);
        binc_application_add_service(app, HTS_SERVICE_UUID);
        binc_application_add_characteristic(
                app,
                HTS_SERVICE_UUID,
                TEMPERATURE_CHAR_UUID,
                GATT_CHR_PROP_INDICATE | GATT_CHR_PROP_WRITE);
        binc_application_add_descriptor(
                app,
                HTS_SERVICE_UUID,
                TEMPERATURE_CHAR_UUID,
                CUD_CHAR,
                GATT_CHR_PROP_READ | GATT_CHR_PROP_WRITE);

        const guint8 cud[] = "hello there";
        GByteArray *cudArray = g_byte_array_sized_new(sizeof(cud));
        g_byte_array_append(cudArray, cud, sizeof(cud));
        binc_application_set_desc_value(app, HTS_SERVICE_UUID, TEMPERATURE_CHAR_UUID, CUD_CHAR, cudArray);

        binc_application_set_char_read_cb(app, &on_local_char_read);
        binc_application_set_char_write_cb(app, &on_local_char_write);
        binc_application_set_char_start_notify_cb(app, &on_local_char_start_notify);
        binc_application_set_char_stop_notify_cb(app, &on_local_char_stop_notify);
        binc_application_set_char_updated_cb(app, &on_local_char_updated);
        binc_adapter_register_application(default_adapter, app);
    } else {
        log_debug("MAIN", "No default_adapter found");
    }

    // Bail out after some time
    g_timeout_add_seconds(600, callback, loop);

    // Start the mainloop
    g_main_loop_run(loop);

    // Clean up mainloop
    g_main_loop_unref(loop);

    // Disconnect from DBus
    g_dbus_connection_close_sync(dbusConnection, NULL, NULL);
    g_object_unref(dbusConnection);
    return 0;
}
