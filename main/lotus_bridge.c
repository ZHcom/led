/* Lotus Lantern BLE integration for r4sGate / ESP-IDF 5.4.x.
 * No separate Bluetooth initialization or scanner: reused Bluedroid callbacks.
 * Controls only the tested FFF0/FFF3 ELK-BLEDOM 9-byte protocol.
 * Five configured slots, one active connection, while four PVVX sensors
 * remain managed by the original passive BLE monitor.
 */
#include "lotus_bridge.h"
#include "lotus_protocol.h"
#include "lotus_ha.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOTUS_OPEN_TIMEOUT_MS 10000
#define LOTUS_SCAN_TIMEOUT_MS 4000
#define LOTUS_CLOSE_TIMEOUT_MS 4000
#define LOTUS_IDLE_DISCONNECT_MS 900
#define LOTUS_COMMAND_SPACING_MS 220
#define LOTUS_ADV_MAX_AGE_MS 30000

static const char *TAG="lotus";
static lotus_manager_t manager;
static SemaphoreHandle_t mutex;
static esp_gatt_if_t client_if=ESP_GATT_IF_NONE;
static esp_mqtt_client_handle_t mqtt;
static char base_topic[32];
static uint8_t bt_type[LOTUS_MAX_LIGHTS];
static bool seen[LOTUS_MAX_LIGHTS];
static int rssi_values[LOTUS_MAX_LIGHTS];
static int64_t last_seen[LOTUS_MAX_LIGHTS];
static int64_t retry_after[LOTUS_MAX_LIGHTS];
static bool publish_pending[LOTUS_MAX_LIGHTS];
static bool have_state[LOTUS_MAX_LIGHTS];
static bool restart_requested;
static int64_t restart_at;

typedef enum {L_IDLE,L_WAIT_SCAN_STOP,L_OPENING,L_DISCOVERING,L_READY,L_CLOSING} link_state_t;
static volatile link_state_t link_state=L_IDLE;
static int active=-1;
static uint16_t connection_id, start_handle, end_handle, write_handle;
static int64_t link_since, last_tx;

static int64_t now_ms(void) {return esp_timer_get_time()/1000;}
static void take(void) {if(mutex)xSemaphoreTake(mutex,portMAX_DELAY);}
static void give(void) {if(mutex)xSemaphoreGive(mutex);}
static bool any_pending(int i) {
    const lotus_light_t *l=&manager.light[i];
    return l->enabled && (l->pending_power||l->pending_rgb||l->pending_brightness);
}
static int next_pending(void) {
    int64_t now=now_ms();
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++)
        if(any_pending(i) && seen[i] && now-last_seen[i]<LOTUS_ADV_MAX_AGE_MS && now>=retry_after[i])return i;
    return -1;
}
static void reset_connection(void) {
    link_state=L_IDLE; active=-1; connection_id=0;
    start_handle=end_handle=write_handle=0; link_since=0; last_tx=0;
}
static void fail_connection(void) {
    if(active>=0 && active<(int)LOTUS_MAX_LIGHTS)retry_after[active]=now_ms()+5000;
    reset_connection();
}
static void begin_open(void) {
    if(active<0 || active>=(int)LOTUS_MAX_LIGHTS || client_if==ESP_GATT_IF_NONE) {reset_connection();return;}
    esp_err_t err=esp_ble_gattc_open(client_if,manager.light[active].mac,
                                      (esp_ble_addr_type_t)bt_type[active],true);
    if(err!=ESP_OK) {ESP_LOGW(TAG,"BLE open refused: %s",esp_err_to_name(err));fail_connection();return;}
    link_state=L_OPENING; link_since=now_ms();
}
static void close_connection(void) {
    if(link_state==L_READY||link_state==L_DISCOVERING) {
        esp_err_t err=esp_ble_gattc_close(client_if,connection_id);
        if(err==ESP_OK) {link_state=L_CLOSING;link_since=now_ms();return;}
    }
    /* If OPEN is in progress we must wait for the callback or its timeout. */
    if(link_state!=L_OPENING)reset_connection();
}

void lotus_init(void) {
    memset(&manager,0,sizeof(manager));
    manager.count=LOTUS_MAX_LIGHTS;
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        manager.light[i].r=manager.light[i].g=manager.light[i].b=255;
        manager.light[i].brightness=100;
    }
    if(!mutex)mutex=xSemaphoreCreateMutex();
    nvs_handle_t h;
    if(nvs_open("lotus",NVS_READWRITE,&h)==ESP_OK){
        uint8_t initialized=0;
        nvs_get_u8(h,"init",&initialized);
        if(!initialized){
            /* First strip was verified in nRF Connect by the owner. */
            nvs_set_str(h,"mac0","BE:37:FA:00:7F:F2");
            nvs_set_u8(h,"init",1);
            nvs_commit(h);
        }
        for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
            char key[8],s[24]={0};size_t len=sizeof(s);uint8_t mac[6];
            snprintf(key,sizeof(key),"mac%d",i);
            if(nvs_get_str(h,key,s,&len)==ESP_OK && lotus_parse_mac(s,mac)){
                memcpy(manager.light[i].mac,mac,6);
                manager.light[i].enabled=true;
            }
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG,"Lotus manager ready (5 slots; no extra BLE scanner)");
}
void lotus_scan_seen(const uint8_t mac[6], esp_ble_addr_type_t type, int rssi){
    if(!mac)return;
    take();
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        if(manager.light[i].enabled && !memcmp(mac,manager.light[i].mac,6)){
            bt_type[i]=(uint8_t)type;seen[i]=true;
            last_seen[i]=now_ms();rssi_values[i]=rssi;
            break;
        }
    }
    give();
}
bool lotus_busy(void) {return link_state!=L_IDLE;}

void lotus_tick(bool scanning,bool *scan_stop_req,bool setting_scan,
                bool ota_active,bool other_ble_busy) {
    if(!mutex||ota_active)return;
    take();
    int64_t now=now_ms();
    if(link_state==L_IDLE && !other_ble_busy && client_if!=ESP_GATT_IF_NONE) {
        int candidate=next_pending();
        if(candidate>=0 && !setting_scan && !*scan_stop_req){
            active=candidate;
            if(scanning) {
                if(esp_ble_gap_stop_scanning()==ESP_OK){
                    *scan_stop_req=true;link_state=L_WAIT_SCAN_STOP;link_since=now;
                } else active=-1;
            } else begin_open();
        }
    } else if(link_state==L_READY) {
        if(now-last_tx>=LOTUS_COMMAND_SPACING_MS && active>=0) {
            lotus_light_t *l=&manager.light[active];
            lotus_frame_t frame;lotus_action_t action=LOTUS_ACTION_NONE;
            if(l->pending_power){action=LOTUS_ACTION_POWER;lotus_frame_power(l->power,&frame);}
            else if(l->pending_rgb){action=LOTUS_ACTION_RGB;lotus_frame_rgb(l->r,l->g,l->b,&frame);}
            else if(l->pending_brightness){action=LOTUS_ACTION_BRIGHTNESS;lotus_frame_brightness_percent(l->brightness,&frame);}
            if(action!=LOTUS_ACTION_NONE){
                esp_err_t err=esp_ble_gattc_write_char(client_if,connection_id,write_handle,
                                   LOTUS_FRAME_SIZE,frame.bytes,ESP_GATT_WRITE_TYPE_NO_RSP,ESP_GATT_AUTH_REQ_NONE);
                if(err==ESP_OK){
                    lotus_manager_ack_action(&manager,(size_t)active,action);
                    publish_pending[active]=true;have_state[active]=true;
                    last_tx=now;
                }else{
                    ESP_LOGW(TAG,"FFF3 write refused: %s",esp_err_to_name(err));
                    close_connection();
                }
            }else if(now-last_tx>=LOTUS_IDLE_DISCONNECT_MS){
                close_connection();
            }
        }
    }else if(link_state!=L_IDLE && (now-link_since)>
                   ((link_state==L_WAIT_SCAN_STOP)?LOTUS_SCAN_TIMEOUT_MS:
                    (link_state==L_CLOSING?LOTUS_CLOSE_TIMEOUT_MS:LOTUS_OPEN_TIMEOUT_MS))){
        ESP_LOGW(TAG,"BLE timeout state=%d",(int)link_state);
        if(link_state==L_DISCOVERING||link_state==L_READY) {
            if(active>=0)retry_after[active]=now+5000;
            close_connection();
        } else if(link_state==L_OPENING) {
            /* Legacy gattc_open has no compatible cancellation on IDF 5.4;
               discard late OPEN_EVT and close any resulting connection. */
            fail_connection();
        } else fail_connection();
    }
    /* Publish only when requested values were accepted by Bluedroid (assumed state).
       These controllers do not reliably report back their physical state. */
    if(mqtt && base_topic[0]) {
        for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++) {
            if(publish_pending[i] && !any_pending(i)){
                char mac[13],topic[96],value[220];
                lotus_mac_compact(manager.light[i].mac,mac);
                snprintf(topic,sizeof(topic),"%s/lotus/%s/state",base_topic,mac);
                if(lotus_ha_state(&manager,i,value,sizeof(value))) {
                    esp_mqtt_client_publish(mqtt,topic,value,0,1,1);
                    publish_pending[i]=false;
                }
            }
        }
    }
    give();
}
bool lotus_scan_stopped(void) {
    if(link_state!=L_WAIT_SCAN_STOP)return false;
    take();begin_open();give();return true;
}

void lotus_gattc_event(esp_gattc_cb_event_t event,esp_gatt_if_t interface,
                       esp_ble_gattc_cb_param_t *p){
    if(!p)return;
    if(event==ESP_GATTC_REG_EVT){
        if(p->reg.app_id==LOTUS_GATTC_APP_ID && p->reg.status==ESP_GATT_OK){
            client_if=interface;ESP_LOGI(TAG,"GATTC registered: %u",(unsigned)interface);
        }
        return;
    }
    if(client_if==ESP_GATT_IF_NONE || interface!=client_if)return;
    take();
    switch(event){
    case ESP_GATTC_OPEN_EVT:
        if(link_state!=L_OPENING || active<0 || memcmp(p->open.remote_bda,manager.light[active].mac,6)){
            if(p->open.status==ESP_GATT_OK)esp_ble_gattc_close(client_if,p->open.conn_id);
            break;
        }
        if(p->open.status!=ESP_GATT_OK){ESP_LOGW(TAG,"Open failed %d",p->open.status);fail_connection();break;}
        connection_id=p->open.conn_id;link_state=L_DISCOVERING;link_since=now_ms();
        if(esp_ble_gattc_send_mtu_req(client_if,connection_id)!=ESP_OK){
            esp_bt_uuid_t u={.len=ESP_UUID_LEN_16,.uuid.uuid16=0xFFF0};
            esp_ble_gattc_search_service(client_if,connection_id,&u);
        }
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        /* r4sGate's Bluedroid cache discovery reports DIS_SRVC_CMPL separately.
           Wait for that event before querying FFF0 (avoids racing the GATT cache). */
        break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if(link_state==L_DISCOVERING && p->dis_srvc_cmpl.conn_id==connection_id){
            esp_bt_uuid_t u={.len=ESP_UUID_LEN_16,.uuid.uuid16=0xFFF0};
            if(esp_ble_gattc_search_service(client_if,connection_id,&u)!=ESP_OK)close_connection();
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if(link_state==L_DISCOVERING && p->search_res.conn_id==connection_id &&
           p->search_res.srvc_id.uuid.len==ESP_UUID_LEN_16 &&
           p->search_res.srvc_id.uuid.uuid.uuid16==0xFFF0){
            start_handle=p->search_res.start_handle;end_handle=p->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if(link_state==L_DISCOVERING && p->search_cmpl.conn_id==connection_id){
            uint16_t count=1;esp_gattc_char_elem_t elem={0};
            esp_bt_uuid_t u={.len=ESP_UUID_LEN_16,.uuid.uuid16=0xFFF3};
            if(start_handle && p->search_cmpl.status==ESP_GATT_OK &&
               esp_ble_gattc_get_char_by_uuid(client_if,connection_id,start_handle,end_handle,u,&elem,&count)==ESP_GATT_OK && count &&
               (elem.properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR)){
                write_handle=elem.char_handle;link_state=L_READY;link_since=now_ms();last_tx=now_ms()-LOTUS_COMMAND_SPACING_MS;
                ESP_LOGI(TAG,"FFF3 found, slot=%d RSSI=%d",active+1,rssi_values[active]);
            }else{
                ESP_LOGW(TAG,"FFF0/FFF3 write-no-response not found");
                if(active>=0)retry_after[active]=now_ms()+10000;
                close_connection();
            }
        }
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        if((link_state==L_READY||link_state==L_CLOSING) && p->write.conn_id==connection_id && p->write.status!=ESP_GATT_OK){
            ESP_LOGW(TAG,"BLE write event status=%u",(unsigned)p->write.status);
        }
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        if(active>=0 && !memcmp(p->disconnect.remote_bda,manager.light[active].mac,6)){
            reset_connection();
        }
        break;
    default:break;
    }
    give();
}

void lotus_mqtt_connected(esp_mqtt_client_handle_t client,const char *prefix){
    if(!client||!prefix)return;
    take();mqtt=client;
    snprintf(base_topic,sizeof(base_topic),"%s",prefix);
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        if(!manager.light[i].enabled)continue;
        char topic[160],json[1000],mac[13],cmd[100];
        if(lotus_ha_discovery(&manager,i,base_topic,topic,sizeof(topic),json,sizeof(json)))
            esp_mqtt_client_publish(client,topic,json,0,1,1);
        lotus_mac_compact(manager.light[i].mac,mac);
        snprintf(cmd,sizeof(cmd),"%s/lotus/%s/set",base_topic,mac);
        esp_mqtt_client_subscribe(client,cmd,0);
        /* Re-send last assumed state ONLY if it was set in this boot. */
        if(have_state[i])publish_pending[i]=true;
    }
    give();
}
void lotus_mqtt_disconnected(void){
    take();mqtt=NULL;give();
}
bool lotus_mqtt_message(const char *topic,int topic_len,const char *payload,int payload_len,const char *prefix){
    if(!topic||!payload||topic_len<=0||payload_len<=0||payload_len>1024||!prefix)return false;
    take();
    bool handled=lotus_ha_parse_command(&manager,prefix,topic,(size_t)topic_len,
                                         payload,(size_t)payload_len);
    give();return handled;
}

/* Keep configuration independent of original r4sGate NVS and protect these
 * endpoints with exactly the same Basic auth check as the existing website.
 * lotus_http_get/post are invoked only through auth-protected wrappers.
 */
static void mac_text(int slot,char out[18]){
    if(!manager.light[slot].enabled){out[0]=0;return;}
    const uint8_t *m=manager.light[slot].mac;
    snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);
}
esp_err_t lotus_http_get(httpd_req_t *req){
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,"<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Lotus Lantern</title><style>body{font:16px system-ui;background:#161b22;color:#e6edf3;max-width:760px;margin:25px auto;padding:12px}input{font-size:1rem;padding:12px;background:#fff;border:0;border-radius:7px;max-width:220px}label{display:block;margin:18px 0}button{background:#238636;color:#fff;border:0;border-radius:7px;padding:12px 20px}a{color:#58a6ff}</style></head><body><h1>Lotus Lantern</h1><p>Five ELK-BLEDOM BLE light slots. Keep BLE monitor enabled for PVVX thermometers.</p><form action='/lotus/save' method='POST'>");
    take();
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        char b[240],mac[18];mac_text(i,mac);
        snprintf(b,sizeof(b),"<label>LED strip %d (MAC): <input name='mac%d' maxlength='17' placeholder='BE:37:FA:00:7F:F2' value='%s'></label>",i+1,i,mac);
        httpd_resp_sendstr_chunk(req,b);
    }
    give();
    httpd_resp_sendstr_chunk(req,"<p>Leave an empty MAC to disable a slot. The second ELK-BLEDDM may need a different protocol; test it before assigning. Save restarts the gateway.</p><button type='submit'>Save and reboot</button></form><p><a href='/'>Back to r4sGate</a></p></body></html>");
    return httpd_resp_sendstr_chunk(req,NULL);
}
static int fromhex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static void decode_form_value(const char *v,size_t n,char *out,size_t cap){
    size_t j=0;
    for(size_t i=0;i<n&&j+1<cap;i++){
        if(v[i]=='%'&&i+2<n){int a=fromhex(v[i+1]),b=fromhex(v[i+2]);if(a>=0&&b>=0){out[j++]=(char)((a<<4)|b);i+=2;continue;}}
        out[j++]=v[i]=='+'?' ':v[i];
    }
    out[j]=0;
}
esp_err_t lotus_http_post(httpd_req_t *req){
    if(req->content_len<=0 || req->content_len>256){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid form size");return ESP_FAIL;}
    char form[257]={0};int pos=0;
    while(pos<req->content_len){
        int n=httpd_req_recv(req,form+pos,req->content_len-pos);
        if(n<=0){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete form");return ESP_FAIL;}
        pos+=n;
    }
    char macs[LOTUS_MAX_LIGHTS][18]={{0}};
    bool fields[LOTUS_MAX_LIGHTS]={0};
    char *save=NULL;
    for(char *part=strtok_r(form,"&",&save);part;part=strtok_r(NULL,"&",&save)){
        if(strncmp(part,"mac",3)||part[3]<'0'||part[3]>'4'||part[4]!='=')continue;
        int slot=part[3]-'0';
        decode_form_value(part+5,strlen(part+5),macs[slot],sizeof(macs[slot]));
        fields[slot]=true;
    }
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        if(!fields[i] || (macs[i][0] && !lotus_parse_mac(macs[i],(uint8_t[6]){0}))){
            httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid MAC address");return ESP_FAIL;
        }
        for(int j=0;j<i;j++)if(macs[i][0] && macs[j][0] && !strcasecmp(macs[i],macs[j])){
            httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Duplicate MAC address");return ESP_FAIL;
        }
    }
    nvs_handle_t h;
    if(nvs_open("lotus",NVS_READWRITE,&h)!=ESP_OK){httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"NVS unavailable");return ESP_FAIL;}
    esp_err_t ret=ESP_OK;
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        char key[8];snprintf(key,sizeof(key),"mac%d",i);
        if(nvs_set_str(h,key,macs[i])!=ESP_OK)ret=ESP_FAIL;
    }
    if(ret==ESP_OK)ret=nvs_commit(h);
    nvs_close(h);
    if(ret!=ESP_OK){httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"NVS write failed");return ESP_FAIL;}
    httpd_resp_set_status(req,"303 See Other");
    httpd_resp_set_hdr(req,"Location","/lotus");
    httpd_resp_send(req,NULL,0);
    restart_at=now_ms()+2000;restart_requested=true;
    return ESP_OK;
}
bool lotus_restart_due(void){return restart_requested && now_ms()>restart_at;}
