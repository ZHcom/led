#pragma once
#include <stdint.h>
#include "esp_gap_ble_api.h"
typedef uint16_t esp_gatt_if_t;
#define ESP_GATT_IF_NONE 0xff
#define ESP_GATT_OK 0
#define ESP_UUID_LEN_16 2
#define ESP_GATT_CHAR_PROP_BIT_WRITE_NR 0x04
#define ESP_GATT_WRITE_TYPE_NO_RSP 1
#define ESP_GATT_AUTH_REQ_NONE 0
#define ESP_GATTC_REG_EVT 1
#define ESP_GATTC_OPEN_EVT 2
#define ESP_GATTC_CFG_MTU_EVT 3
#define ESP_GATTC_SEARCH_RES_EVT 4
#define ESP_GATTC_SEARCH_CMPL_EVT 5
#define ESP_GATTC_WRITE_CHAR_EVT 6
#define ESP_GATTC_DISCONNECT_EVT 7
#define ESP_GATTC_DIS_SRVC_CMPL_EVT 8
typedef int esp_gattc_cb_event_t;
typedef struct {uint16_t len;union {uint16_t uuid16;}uuid;} esp_bt_uuid_t;
typedef struct {uint16_t char_handle;uint8_t properties;} esp_gattc_char_elem_t;
typedef struct {uint8_t app_id;int status;} reg_t;
typedef struct {uint8_t remote_bda[6];int status;uint16_t conn_id;} open_t;
typedef struct {uint16_t conn_id;} cfg_mtu_t;
typedef struct {uint16_t conn_id;struct {esp_bt_uuid_t uuid;} srvc_id;uint16_t start_handle,end_handle;} search_res_t;
typedef struct {uint16_t conn_id;int status;} search_cmpl_t;
typedef struct {uint16_t conn_id;int status;} dis_srvc_cmpl_t;
typedef struct {uint16_t conn_id;int status;} write_t;
typedef struct {uint8_t remote_bda[6];} disconnect_t;
typedef union {reg_t reg;open_t open;cfg_mtu_t cfg_mtu;search_res_t search_res;search_cmpl_t search_cmpl;dis_srvc_cmpl_t dis_srvc_cmpl;write_t write;disconnect_t disconnect;} esp_ble_gattc_cb_param_t;
esp_err_t esp_ble_gattc_open(esp_gatt_if_t,uint8_t[6],esp_ble_addr_type_t,int);
esp_err_t esp_ble_gattc_close(esp_gatt_if_t,uint16_t);
esp_err_t esp_ble_gattc_cancel_open(esp_gatt_if_t,uint8_t[6],int);
esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t,uint16_t,uint16_t,uint16_t,uint8_t*,int,int);
esp_err_t esp_ble_gattc_send_mtu_req(esp_gatt_if_t,uint16_t);
esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t,uint16_t,esp_bt_uuid_t*);
int esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t,uint16_t,uint16_t,uint16_t,esp_bt_uuid_t,esp_gattc_char_elem_t*,uint16_t*);
