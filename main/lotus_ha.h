#ifndef LOTUS_HA_H
#define LOTUS_HA_H
#include "lotus_protocol.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * MQTT adapter intended for use within the existing r4sGate mqtt_event_handler.
 * Requires ESP-IDF cJSON component. Does NOT create an MQTT connection.
 * The caller publishes the returned retained Discovery JSON once connected.
 */

bool lotus_ha_discovery(const lotus_manager_t *manager, size_t index,
                       const char *gateway_prefix,
                       char *topic, size_t topic_len,
                       char *json, size_t json_len);

bool lotus_ha_parse_command(lotus_manager_t *manager,
                            const char *gateway_prefix,
                            const char *topic, size_t topic_len,
                            const char *payload, size_t payload_len);

bool lotus_ha_state(const lotus_manager_t *manager, size_t index,
                    char *json, size_t json_len);
#endif
