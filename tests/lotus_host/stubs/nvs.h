#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
typedef unsigned int nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_get_u8(nvs_handle_t,const char*,uint8_t*);
esp_err_t nvs_set_u8(nvs_handle_t,const char*,uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_get_str(nvs_handle_t,const char*,char*,size_t*);
esp_err_t nvs_set_str(nvs_handle_t,const char*,const char*);
void nvs_close(nvs_handle_t);
