#ifndef R4SGATE_LOTUS_BRIDGE_H
#define R4SGATE_LOTUS_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_http_server.h"
#include "mqtt_client.h"

/* One extra GATTC profile, reusing r4sGate's existing Bluedroid stack. */
#define LOTUS_GATTC_APP_ID 5

void lotus_init(void);
/* RTC-retained crash breadcrumbs, visible on /lotus, no credentials stored. */
void lotus_diag_event(const char *stage, int a, int b);
bool lotus_enabled(void);
void lotus_scan_seen(const uint8_t mac[6], esp_ble_addr_type_t type, int rssi);
bool lotus_busy(void);
/* Called from the existing main loop, not an additional BLE scanner. */
void lotus_tick(bool scanning, bool *scan_stop_req, bool setting_scan,
                bool ota_active, bool other_ble_busy);
/* Return true if Lotus took ownership of the completed scan-stop event. */
bool lotus_scan_stopped(void);
/* Called by r4sGate's existing GATTC dispatcher. */
void lotus_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
                       esp_ble_gattc_cb_param_t *param);
void lotus_mqtt_connected(esp_mqtt_client_handle_t client, const char *prefix);
void lotus_mqtt_disconnected(void);
bool lotus_mqtt_message(const char *topic, int topic_len,
                        const char *payload, int payload_len,
                        const char *prefix);
esp_err_t lotus_http_get(httpd_req_t *request);
esp_err_t lotus_http_post(httpd_req_t *request);
bool lotus_restart_due(void);

#endif
