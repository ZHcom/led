#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef uint8_t esp_ble_addr_type_t;
typedef uint8_t esp_bd_addr_t[6];
esp_err_t esp_ble_gap_stop_scanning(void);
