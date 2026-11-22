#include "lotus_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_frame(const lotus_frame_t *actual, const uint8_t expected[9]) {
    if (memcmp(actual->bytes, expected, 9) != 0) {
        for (int i = 0; i < 9; ++i) printf("%02X ", actual->bytes[i]);
        puts("");
        assert(0 && "Unexpected frame bytes");
    }
}

int main(void) {
    lotus_frame_t frame;
    lotus_frame_power(false, &frame);
    check_frame(&frame, (const uint8_t[]){0x7E,0x00,0x04,0x00,0x00,0x00,0xFF,0x00,0xEF});
    lotus_frame_power(true, &frame);
    check_frame(&frame, (const uint8_t[]){0x7E,0x00,0x04,0xF0,0x00,0x01,0xFF,0x00,0xEF});
    lotus_frame_rgb(255,0,0,&frame);
    check_frame(&frame, (const uint8_t[]){0x7E,0x00,0x05,0x03,0xFF,0x00,0x00,0x00,0xEF});
    lotus_frame_brightness_percent(20,&frame);
    check_frame(&frame, (const uint8_t[]){0x7E,0x00,0x01,0x14,0x00,0x00,0x00,0x00,0xEF});
    assert(lotus_ha_brightness_to_percent(0) == 0);
    assert(lotus_ha_brightness_to_percent(255) == 100);
    lotus_manager_t manager;
    lotus_manager_reset(&manager);
    assert(lotus_manager_add(&manager, "BE:37:FA:00:7F:F2"));
    assert(!lotus_manager_add(&manager, "BE37FA007FF2"));
    assert(lotus_manager_add(&manager, "BE:27:32:01:5D:51"));
    assert(!lotus_manager_add(&manager, "invalid"));
    assert(lotus_manager_power(&manager, 0, true));
    assert(lotus_manager_rgb(&manager,0,255,0,0));
    assert(lotus_manager_brightness(&manager,0,20));
    size_t ix=88;
    assert(lotus_manager_next(&manager,&ix,&frame) == LOTUS_ACTION_POWER && ix == 0);
    lotus_manager_ack_action(&manager,ix,LOTUS_ACTION_POWER);
    assert(lotus_manager_next(&manager,&ix,&frame) == LOTUS_ACTION_RGB && ix == 0);
    lotus_manager_ack_action(&manager,ix,LOTUS_ACTION_RGB);
    assert(lotus_manager_next(&manager,&ix,&frame) == LOTUS_ACTION_BRIGHTNESS && ix == 0);
    lotus_manager_ack_action(&manager,ix,LOTUS_ACTION_BRIGHTNESS);
    assert(lotus_manager_next(&manager,&ix,&frame) == LOTUS_ACTION_NONE);
    assert(manager.count == 2 && LOTUS_MAX_LIGHTS == 5);
    puts("OK: validated 4 frames, MACs, 5-slot manager, queue ordering");
    return 0;
}
