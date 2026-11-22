#include "lotus_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool lotus_parse_mac(const char *str, uint8_t out[6])
{
    if (!str || !out) return false;
    /* Accept either BE:37:FA:00:7F:F2 or BE37FA007FF2. */
    size_t length = strlen(str);
    if (length != 17 && length != 12) return false;
    for (size_t i = 0; i < 6; ++i) {
        size_t k = (length == 17) ? i * 3 : i * 2;
        if (length == 17 && i != 5 && str[k + 2] != ':') return false;
        int hi = hex_value(str[k]), lo = hex_value(str[k + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void lotus_mac_compact(const uint8_t mac[6], char out[13])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 6; ++i) {
        out[i * 2] = digits[mac[i] >> 4];
        out[i * 2 + 1] = digits[mac[i] & 0xf];
    }
    out[12] = 0;
}

void lotus_frame_power(bool on, lotus_frame_t *out)
{
    const uint8_t frame[LOTUS_FRAME_SIZE] = {0x7E,0x00,0x04,on?0xF0:0x00,0x00,on?0x01:0x00,0xFF,0x00,0xEF};
    memcpy(out->bytes, frame, LOTUS_FRAME_SIZE);
}

void lotus_frame_rgb(uint8_t red, uint8_t green, uint8_t blue, lotus_frame_t *out)
{
    const uint8_t frame[LOTUS_FRAME_SIZE] = {0x7E,0x00,0x05,0x03,red,green,blue,0x00,0xEF};
    memcpy(out->bytes, frame, LOTUS_FRAME_SIZE);
}

void lotus_frame_brightness_percent(uint8_t brightness, lotus_frame_t *out)
{
    if (brightness > 100) brightness = 100;
    const uint8_t frame[LOTUS_FRAME_SIZE] = {0x7E,0x00,0x01,brightness,0x00,0x00,0x00,0x00,0xEF};
    memcpy(out->bytes, frame, LOTUS_FRAME_SIZE);
}

uint8_t lotus_ha_brightness_to_percent(uint8_t value)
{
    /* Rounded to nearest integer; avoids 20% mapping to 19%. */
    return (uint8_t)(((unsigned)value * 100u + 127u) / 255u);
}

void lotus_manager_reset(lotus_manager_t *manager)
{
    if (manager) memset(manager, 0, sizeof(*manager));
}

bool lotus_manager_add(lotus_manager_t *manager, const char *mac)
{
    uint8_t raw[6];
    if (!manager || manager->count >= LOTUS_MAX_LIGHTS || !lotus_parse_mac(mac, raw)) return false;
    for (size_t i = 0; i < manager->count; ++i) {
        if (memcmp(raw, manager->light[i].mac, 6) == 0) return false;
    }
    lotus_light_t *device = &manager->light[manager->count++];
    memset(device, 0, sizeof(*device));
    memcpy(device->mac, raw, 6);
    device->enabled = true;
    device->r = device->g = device->b = 255;
    device->brightness = 100;
    return true;
}

bool lotus_manager_power(lotus_manager_t *manager, size_t index, bool on)
{
    if (!manager || index >= manager->count || !manager->light[index].enabled) return false;
    lotus_light_t *device = &manager->light[index];
    device->power = on;
    device->pending_power = true;
    return true;
}

bool lotus_manager_rgb(lotus_manager_t *manager, size_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!manager || index >= manager->count || !manager->light[index].enabled) return false;
    lotus_light_t *device = &manager->light[index];
    device->r = red; device->g = green; device->b = blue;
    device->pending_rgb = true;
    return true;
}

bool lotus_manager_brightness(lotus_manager_t *manager, size_t index, uint8_t percent)
{
    if (!manager || index >= manager->count || !manager->light[index].enabled) return false;
    lotus_light_t *device = &manager->light[index];
    device->brightness = percent > 100 ? 100 : percent;
    device->pending_brightness = true;
    return true;
}

lotus_action_t lotus_manager_next(lotus_manager_t *manager, size_t *index_out, lotus_frame_t *frame_out)
{
    if (!manager || !frame_out || !index_out || manager->count == 0) return LOTUS_ACTION_NONE;
    for (size_t step = 0; step < manager->count; ++step) {
        size_t index = (manager->round_robin + step) % manager->count;
        lotus_light_t *device = &manager->light[index];
        if (!device->enabled) continue;
        if (device->pending_power) {
            lotus_frame_power(device->power, frame_out);
            *index_out = index;
            return LOTUS_ACTION_POWER;
        }
        if (device->pending_rgb) {
            lotus_frame_rgb(device->r, device->g, device->b, frame_out);
            *index_out = index;
            return LOTUS_ACTION_RGB;
        }
        if (device->pending_brightness) {
            lotus_frame_brightness_percent(device->brightness, frame_out);
            *index_out = index;
            return LOTUS_ACTION_BRIGHTNESS;
        }
    }
    return LOTUS_ACTION_NONE;
}

void lotus_manager_ack_action(lotus_manager_t *manager, size_t index, lotus_action_t action)
{
    if (!manager || index >= manager->count) return;
    lotus_light_t *device = &manager->light[index];
    switch (action) {
        case LOTUS_ACTION_POWER: device->pending_power = false; break;
        case LOTUS_ACTION_RGB: device->pending_rgb = false; break;
        case LOTUS_ACTION_BRIGHTNESS: device->pending_brightness = false; break;
        default: return;
    }
    manager->round_robin = (index + 1) % manager->count;
}
