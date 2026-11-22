#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include "lotus_bridge.h"
#define MAXN 5
static int64_t t_ms=1000;
static int n_stop,n_open,n_close,n_write,n_publish,n_subscribe;
static uint8_t last_frame[9];
static char nvs_vals[MAXN][24];
static int has_init;
const char *esp_err_to_name(esp_err_t e){(void)e;return "mock";}
int64_t esp_timer_get_time(void){return t_ms*1000;}
void *xSemaphoreCreateMutex(void){return (void*)1;}
int xSemaphoreTake(void *s,unsigned t){(void)s;(void)t;return 1;}
int xSemaphoreGive(void *s){(void)s;return 1;}
esp_err_t esp_ble_gap_stop_scanning(void){n_stop++;return ESP_OK;}
esp_err_t esp_ble_gattc_open(esp_gatt_if_t g,uint8_t mac[6],esp_ble_addr_type_t type,int direct){(void)g;(void)type;(void)direct;assert(mac[0]==0xbe);n_open++;return ESP_OK;}
esp_err_t esp_ble_gattc_close(esp_gatt_if_t g,uint16_t conn){(void)g;(void)conn;n_close++;return ESP_OK;}
esp_err_t esp_ble_gattc_cancel_open(esp_gatt_if_t g,uint8_t mac[6],int direct){(void)g;(void)mac;(void)direct;return ESP_OK;}
esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t g,uint16_t conn,uint16_t handle,uint16_t n,uint8_t *buf,int type,int auth){(void)g;(void)conn;(void)type;(void)auth;assert(handle==7 && n==9);memcpy(last_frame,buf,n);n_write++;return ESP_OK;}
esp_err_t esp_ble_gattc_send_mtu_req(esp_gatt_if_t g,uint16_t c){(void)g;(void)c;return ESP_OK;}
esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t g,uint16_t c,esp_bt_uuid_t *u){(void)g;(void)c;assert(u->uuid.uuid16==0xfff0);return ESP_OK;}
int esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t g,uint16_t c,uint16_t start,uint16_t end,esp_bt_uuid_t uuid,esp_gattc_char_elem_t *out,uint16_t *count){(void)g;(void)c;(void)start;(void)end;assert(uuid.uuid.uuid16==0xfff3);*count=1;out->char_handle=7;out->properties=ESP_GATT_CHAR_PROP_BIT_WRITE_NR;return ESP_GATT_OK;}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c,const char*t,const char*p,int l,int q,int r){(void)c;(void)p;(void)l;(void)q;(void)r;assert(strstr(t,"lotus")||strstr(t,"homeassistant"));n_publish++;return 1;}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c,const char*t,int q){(void)c;(void)q;assert(strstr(t,"lotus"));n_subscribe++;return 1;}
esp_err_t nvs_open(const char *n,int mode,unsigned *h){(void)mode;assert(!strcmp(n,"lotus"));*h=1;return ESP_OK;}
esp_err_t nvs_get_u8(unsigned h,const char *k,uint8_t *v){(void)h;assert(!strcmp(k,"init"));*v=has_init;return ESP_OK;}
esp_err_t nvs_set_u8(unsigned h,const char *k,uint8_t v){(void)h;assert(!strcmp(k,"init"));has_init=v;return ESP_OK;}
esp_err_t nvs_commit(unsigned h){(void)h;return ESP_OK;}
esp_err_t nvs_get_str(unsigned h,const char *k,char *s,size_t *sz){(void)h;int n=-1;if(sscanf(k,"mac%d",&n)!=1 || n<0 || n>=5 || !nvs_vals[n][0])return ESP_FAIL;assert(*sz>=strlen(nvs_vals[n])+1);strcpy(s,nvs_vals[n]);*sz=strlen(s)+1;return ESP_OK;}
esp_err_t nvs_set_str(unsigned h,const char *k,const char *v){(void)h;int n=-1;sscanf(k,"mac%d",&n);assert(n>=0&&n<5);snprintf(nvs_vals[n],sizeof(nvs_vals[n]),"%s",v);return ESP_OK;}
void nvs_close(unsigned h){(void)h;}
esp_err_t httpd_resp_set_type(httpd_req_t*r,const char*s){(void)r;(void)s;return ESP_OK;}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t*r,const char*s){(void)r;(void)s;return ESP_OK;}
esp_err_t httpd_resp_send_err(httpd_req_t*r,int e,const char*s){(void)r;(void)e;(void)s;return ESP_OK;}
esp_err_t httpd_resp_set_status(httpd_req_t*r,const char*s){(void)r;(void)s;return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){(void)r;(void)k;(void)v;return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t*r,const char*s,int n){(void)r;(void)s;(void)n;return ESP_OK;}
int httpd_req_recv(httpd_req_t*r,char*s,size_t n){(void)r;(void)s;(void)n;return -1;}
int main(void){
 lotus_init();assert(has_init && !strcmp(nvs_vals[0],"BE:37:FA:00:7F:F2"));
 esp_ble_gattc_cb_param_t evt={0};evt.reg.app_id=LOTUS_GATTC_APP_ID;evt.reg.status=ESP_GATT_OK;
 lotus_gattc_event(ESP_GATTC_REG_EVT,42,&evt);
 lotus_mqtt_connected((void*)1,"r4s0");assert(n_publish==1 && n_subscribe==1);
 const char *cmd="{\"state\":\"ON\",\"brightness\":51,\"color\":{\"r\":255,\"g\":0,\"b\":0}}";
 const char *topic="r4s0/lotus/be37fa007ff2/set";
 assert(lotus_mqtt_message(topic,(int)strlen(topic),cmd,(int)strlen(cmd),"r4s0"));
 uint8_t mac[]={0xbe,0x37,0xfa,0,0x7f,0xf2};lotus_scan_seen(mac,1,-75);
 bool stop=false;lotus_tick(true,&stop,false,false,false);assert(n_stop==1 && stop && lotus_busy());
 assert(lotus_scan_stopped() && n_open==1);
 memset(&evt,0,sizeof(evt));evt.open.status=ESP_GATT_OK;evt.open.conn_id=17;memcpy(evt.open.remote_bda,mac,6);
 lotus_gattc_event(ESP_GATTC_OPEN_EVT,42,&evt);
 memset(&evt,0,sizeof(evt));evt.cfg_mtu.conn_id=17;lotus_gattc_event(ESP_GATTC_CFG_MTU_EVT,42,&evt);
 memset(&evt,0,sizeof(evt));evt.dis_srvc_cmpl.conn_id=17;evt.dis_srvc_cmpl.status=ESP_GATT_OK;lotus_gattc_event(ESP_GATTC_DIS_SRVC_CMPL_EVT,42,&evt);
 memset(&evt,0,sizeof(evt));evt.search_res.conn_id=17;evt.search_res.srvc_id.uuid.len=ESP_UUID_LEN_16;evt.search_res.srvc_id.uuid.uuid.uuid16=0xfff0;evt.search_res.start_handle=1;evt.search_res.end_handle=9;
 lotus_gattc_event(ESP_GATTC_SEARCH_RES_EVT,42,&evt);
 memset(&evt,0,sizeof(evt));evt.search_cmpl.conn_id=17;evt.search_cmpl.status=ESP_GATT_OK;
 lotus_gattc_event(ESP_GATTC_SEARCH_CMPL_EVT,42,&evt);
 lotus_tick(false,&stop,false,false,false);assert(n_write==1 && last_frame[2]==4 && last_frame[3]==0xf0);
 t_ms+=300;lotus_tick(false,&stop,false,false,false);assert(n_write==2 && last_frame[2]==5 && last_frame[4]==0xff);
 t_ms+=300;lotus_tick(false,&stop,false,false,false);assert(n_write==3 && last_frame[2]==1 && last_frame[3]==20);
 assert(n_publish==2); // discovery + state (assumed)
 t_ms+=1200;lotus_tick(false,&stop,false,false,false);assert(n_close==1);
 memset(&evt,0,sizeof(evt));memcpy(evt.disconnect.remote_bda,mac,6);lotus_gattc_event(ESP_GATTC_DISCONNECT_EVT,42,&evt);
 assert(!lotus_busy());puts("OK: Lotus GATT lifecycle, MQTT discovery, 3 command writes and disconnect");
}
