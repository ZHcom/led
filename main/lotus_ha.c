/* Compile this source only inside the ESP-IDF project, which ships cJSON. */
#include "lotus_ha.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool print_json(cJSON *root, char *out, size_t cap)
{
    if (!root || !out || cap == 0) return false;
    char *json = cJSON_PrintUnformatted(root);
    if (!json) return false;
    size_t needed = strlen(json) + 1;
    bool ok = needed <= cap;
    if (ok) memcpy(out, json, needed);
    cJSON_free(json);
    return ok;
}

static void set_text(cJSON *obj, const char *key, const char *value)
{
    cJSON_AddStringToObject(obj, key, value);
}

bool lotus_ha_discovery(const lotus_manager_t *manager, size_t index,
                       const char *gateway_prefix, char *topic, size_t topic_len,
                       char *json, size_t json_len)
{
    if (!manager || index >= manager->count || !gateway_prefix || !topic || !json) return false;
    const lotus_light_t *light = &manager->light[index];
    char mac[13], name[32], unique_id[48], command_topic[128], state_topic[128];
    lotus_mac_compact(light->mac, mac);
    snprintf(name, sizeof(name), "Lotus %u", (unsigned)(index + 1));
    snprintf(unique_id, sizeof(unique_id), "r4sgate_lotus_%s", mac);
    int count = snprintf(topic, topic_len, "homeassistant/light/%s/config", unique_id);
    if (count < 0 || (size_t)count >= topic_len) return false;
    snprintf(command_topic, sizeof(command_topic), "%s/lotus/%s/set", gateway_prefix, mac);
    snprintf(state_topic, sizeof(state_topic), "%s/lotus/%s/state", gateway_prefix, mac);
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    set_text(root, "name", name);
    set_text(root, "unique_id", unique_id);
    set_text(root, "schema", "json");
    set_text(root, "command_topic", command_topic);
    set_text(root, "state_topic", state_topic);
    cJSON *modes = cJSON_AddArrayToObject(root, "supported_color_modes");
    if (modes) cJSON_AddItemToArray(modes, cJSON_CreateString("rgb"));
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    if (device) {
        char device_id[48];
        snprintf(device_id, sizeof(device_id), "r4sgate_lotus_controller_%s",mac);
        cJSON *ids=cJSON_AddArrayToObject(device, "identifiers");
        if (ids) cJSON_AddItemToArray(ids,cJSON_CreateString(device_id));
        set_text(device,"name",name);
        set_text(device,"manufacturer","Lotus Lantern / ELK");
        set_text(device,"model","ELK-BLEDOM (protocol pending second-device validation)");
    }
    bool ok = print_json(root, json, json_len);
    cJSON_Delete(root);
    return ok;
}

static bool is_valid_number(const cJSON *item)
{
    return cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 255.0;
}

bool lotus_ha_parse_command(lotus_manager_t *manager,
                            const char *gateway_prefix,
                            const char *topic, size_t topic_len,
                            const char *payload, size_t payload_len)
{
    if (!manager || !gateway_prefix || !topic || !payload || payload_len == 0 || payload_len > 1024) return false;
    size_t matched_index = manager->count;
    for (size_t i = 0; i < manager->count; ++i) {
        char expected[128], mac[13];
        lotus_mac_compact(manager->light[i].mac, mac);
        int n = snprintf(expected, sizeof(expected), "%s/lotus/%s/set", gateway_prefix, mac);
        if (n > 0 && (size_t)n == topic_len && memcmp(expected, topic, topic_len) == 0) {
            matched_index = i; break;
        }
    }
    if (matched_index == manager->count) return false;
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root) return false;
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root,"state");
    cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root,"brightness");
    cJSON *color = cJSON_GetObjectItemCaseSensitive(root,"color");
    bool accepted = false;
    if (cJSON_IsString(state)) {
        if (strcmp(state->valuestring,"ON")==0) {
            lotus_manager_power(manager,matched_index,true); accepted=true;
        } else if (strcmp(state->valuestring,"OFF")==0) {
            lotus_manager_power(manager,matched_index,false); accepted=true;
        }
    }
    if (is_valid_number(brightness)) {
        uint8_t value=(uint8_t)brightness->valueint;
        lotus_manager_brightness(manager,matched_index,lotus_ha_brightness_to_percent(value));
        accepted=true;
    }
    if (cJSON_IsObject(color)) {
        const cJSON *r=cJSON_GetObjectItemCaseSensitive(color,"r");
        const cJSON *g=cJSON_GetObjectItemCaseSensitive(color,"g");
        const cJSON *b=cJSON_GetObjectItemCaseSensitive(color,"b");
        if (is_valid_number(r)&&is_valid_number(g)&&is_valid_number(b)) {
            lotus_manager_rgb(manager,matched_index,(uint8_t)r->valueint,(uint8_t)g->valueint,(uint8_t)b->valueint);
            accepted=true;
        }
    }
    cJSON_Delete(root);
    return accepted;
}

bool lotus_ha_state(const lotus_manager_t *manager, size_t index, char *json, size_t json_len)
{
    if (!manager || index >= manager->count) return false;
    const lotus_light_t *light = &manager->light[index];
    cJSON *root=cJSON_CreateObject();
    if (!root) return false;
    set_text(root,"state",light->power?"ON":"OFF");
    /* 0..100 -> HA's 0..255 with rounding */
    cJSON_AddNumberToObject(root,"brightness", ((unsigned)light->brightness*255u+50u)/100u);
    cJSON *color = cJSON_AddObjectToObject(root,"color");
    if (color) {
        cJSON_AddNumberToObject(color,"r",light->r);
        cJSON_AddNumberToObject(color,"g",light->g);
        cJSON_AddNumberToObject(color,"b",light->b);
    }
    bool ok=print_json(root,json,json_len);
    cJSON_Delete(root);
    return ok;
}
