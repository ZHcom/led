#ifndef LOTUS_PROTOCOL_H
#define LOTUS_PROTOCOL_H

/*
 * ELK-BLEDOM protocol core for r4sGate (ESP-IDF 5.4.2 / ESP32).
 * These 9-byte frames were confirmed by the device owner on
 * BE:37:FA:00:7F:F2 using nRF Connect (FFF0 / FFF3, write no response).
 * No ESP-IDF dependencies here: can be compiled/tested with GCC.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOTUS_MAX_LIGHTS 5u
#define LOTUS_FRAME_SIZE 9u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t bytes[LOTUS_FRAME_SIZE];
} lotus_frame_t;

typedef struct {
    uint8_t mac[6];
    bool enabled;
    bool power;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness; /* 0..100 */
    bool pending_power;
    bool pending_rgb;
    bool pending_brightness;
} lotus_light_t;

typedef struct {
    lotus_light_t light[LOTUS_MAX_LIGHTS];
    size_t count;
    size_t round_robin;
} lotus_manager_t;

typedef enum {
    LOTUS_ACTION_NONE = 0,
    LOTUS_ACTION_POWER,
    LOTUS_ACTION_RGB,
    LOTUS_ACTION_BRIGHTNESS
} lotus_action_t;

bool lotus_parse_mac(const char *str, uint8_t out[6]);
void lotus_mac_compact(const uint8_t mac[6], char out[13]);
void lotus_frame_power(bool on, lotus_frame_t *out);
void lotus_frame_rgb(uint8_t red, uint8_t green, uint8_t blue, lotus_frame_t *out);
void lotus_frame_brightness_percent(uint8_t brightness, lotus_frame_t *out);
uint8_t lotus_ha_brightness_to_percent(uint8_t ha_brightness);

void lotus_manager_reset(lotus_manager_t *manager);
bool lotus_manager_add(lotus_manager_t *manager, const char *mac);
bool lotus_manager_power(lotus_manager_t *manager, size_t index, bool on);
bool lotus_manager_rgb(lotus_manager_t *manager, size_t index, uint8_t red, uint8_t green, uint8_t blue);
bool lotus_manager_brightness(lotus_manager_t *manager, size_t index, uint8_t brightness_percent);
/*
 * Get one pending action without removing it. Call lotus_manager_ack_action()
 * ONLY after transport acceptance. If a BLE write fails, retry later.
 * Never treat successful write as device-reported confirmation.
 */
lotus_action_t lotus_manager_next(lotus_manager_t *manager, size_t *index_out, lotus_frame_t *frame_out);
void lotus_manager_ack_action(lotus_manager_t *manager, size_t index, lotus_action_t action);

#ifdef __cplusplus
}
#endif
#endif
