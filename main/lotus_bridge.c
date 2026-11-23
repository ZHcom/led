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
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_attr.h"

#define LOTUS_OPEN_TIMEOUT_MS 10000
#define LOTUS_SCAN_TIMEOUT_MS 4000
#define LOTUS_CLOSE_TIMEOUT_MS 4000
#define LOTUS_IDLE_DISCONNECT_MS 900
#define LOTUS_COMMAND_SPACING_MS 220
#define LOTUS_ADV_MAX_AGE_MS 30000
#define LOTUS_TRIAL_MS 120000
#define LOTUS_EVENT_QUEUE_LEN 16
#define LOTUS_SCAN_CANDIDATES 10
#define LOTUS_MAX_WEB_FORM 700
#define LOTUS_RETRY_MIN_MS 4000
#define LOTUS_RETRY_MAX_MS 60000

/* v3: crash breadcrumbs in RTC no-init RAM. No flash write for each event.
 * RTC memory is normally retained across SW/PANIC resets but not power loss.
 * Only stage markers, BLE status/error codes are recorded (no secrets).
 */
#define LOTUS_DIAG_MAGIC 0x4c4f5433u
#define LOTUS_DIAG_CAP 32u
typedef struct {
    uint32_t boot;
    uint32_t at_ms;
    int32_t a, b;
    char stage[36];
} lotus_diag_entry_t;
typedef struct {
    uint32_t magic, inverted_magic;
    uint32_t boot, head;
    uint32_t last_reset;
    char previous_stage[36];
    lotus_diag_entry_t entries[LOTUS_DIAG_CAP];
} lotus_diag_store_t;
RTC_NOINIT_ATTR static lotus_diag_store_t lotus_diag_rtc;
static portMUX_TYPE lotus_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static bool lotus_diag_ready;

static const char *lotus_reset_name(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default: return "OTHER";
    }
}

void lotus_diag_event(const char *stage, int a, int b) {
    if (!lotus_diag_ready || !stage) return;
    /* Constant-time and non-allocating. Safe to call from Bluedroid task. */
    uint32_t ms=(uint32_t)(esp_timer_get_time()/1000);
    char safe_stage[36];
    snprintf(safe_stage,sizeof(safe_stage),"%s",stage);
    portENTER_CRITICAL(&lotus_diag_mux);
    uint32_t ix=lotus_diag_rtc.head++ % LOTUS_DIAG_CAP;
    lotus_diag_entry_t *e=&lotus_diag_rtc.entries[ix];
    e->boot=lotus_diag_rtc.boot;
    e->at_ms=ms; e->a=a; e->b=b;
    memcpy(e->stage,safe_stage,sizeof(e->stage));
    portEXIT_CRITICAL(&lotus_diag_mux);
}

static void lotus_diag_boot(void) {
    const esp_reset_reason_t reason=esp_reset_reason();
    /* No NVS writes here: previous entries survive a normal panic reset. */
    if (lotus_diag_rtc.magic!=LOTUS_DIAG_MAGIC ||
        lotus_diag_rtc.inverted_magic!=~LOTUS_DIAG_MAGIC ||
        lotus_diag_rtc.head>1000000u) {
        memset(&lotus_diag_rtc,0,sizeof(lotus_diag_rtc));
        lotus_diag_rtc.magic=LOTUS_DIAG_MAGIC;
        lotus_diag_rtc.inverted_magic=~LOTUS_DIAG_MAGIC;
    }
    if (lotus_diag_rtc.head>0) {
        lotus_diag_entry_t *last=&lotus_diag_rtc.entries[(lotus_diag_rtc.head-1)%LOTUS_DIAG_CAP];
        snprintf(lotus_diag_rtc.previous_stage,sizeof(lotus_diag_rtc.previous_stage),
                 "%s",last->stage);
    }
    ++lotus_diag_rtc.boot;
    lotus_diag_rtc.last_reset=(uint32_t)reason;
    lotus_diag_ready=true;
    lotus_diag_event("BOOT",(int)reason,0);
}

/* Build one plain text line at a time; no multi-KB copies on web task stack. */
static void lotus_diag_render(httpd_req_t *req, bool html) {
    uint32_t head, boot, reset;
    char previous[36];
    portENTER_CRITICAL(&lotus_diag_mux);
    head=lotus_diag_rtc.head; boot=lotus_diag_rtc.boot; reset=lotus_diag_rtc.last_reset;
    memcpy(previous,lotus_diag_rtc.previous_stage,sizeof(previous));
    portEXIT_CRITICAL(&lotus_diag_mux);
    char row[192];
    if (html) {
        httpd_resp_sendstr_chunk(req,"<h2>Crash / Bluetooth diagnostic log</h2><p>Saved in RTC memory across most software and panic resets; a power cut can clear it. Does not contain a full CPU backtrace.</p><pre id='lotus-log' style='white-space:pre-wrap;overflow-wrap:anywhere;background:#0d1117;padding:14px;border-radius:8px'>");
    }
    snprintf(row,sizeof(row),"Current boot: %"PRIu32" | Last reset: %s (%"PRIu32")\nPrevious boot last step: %s\n\n",
             boot,lotus_reset_name((esp_reset_reason_t)reset),reset,previous[0]?previous:"not retained");
    httpd_resp_sendstr_chunk(req,row);
    uint32_t start=(head>LOTUS_DIAG_CAP)?head-LOTUS_DIAG_CAP:0;
    for(uint32_t n=start;n<head;n++) {
        lotus_diag_entry_t e;
        portENTER_CRITICAL(&lotus_diag_mux);
        e=lotus_diag_rtc.entries[n%LOTUS_DIAG_CAP];
        portEXIT_CRITICAL(&lotus_diag_mux);
        snprintf(row,sizeof(row),"boot %"PRIu32" +%"PRIu32" ms | %-32s | a=%"PRId32" b=%"PRId32"\n",
                 e.boot,e.at_ms,e.stage,e.a,e.b);
        httpd_resp_sendstr_chunk(req,row);
    }
    if (html) httpd_resp_sendstr_chunk(req,"</pre><p><a href='/lotus?log=1'>Open text log for copying</a> | <a href='/lotus'>Refresh page</a>. Log refreshes every 5 seconds while this page is open.</p><script>setInterval(function(){fetch('/lotus?log=1',{cache:'no-store'}).then(function(r){if(!r.ok)throw Error(r.status);return r.text()}).then(function(s){document.getElementById('lotus-log').textContent=s}).catch(function(){})},5000)</script>");
}

static const char *TAG="lotus";
static lotus_manager_t manager;
static SemaphoreHandle_t mutex;
/* Callback -> main loop: no mutex waits, BLE API calls, or MQTT from Bluedroid. */
typedef struct {
    esp_gattc_cb_event_t event;
    esp_gatt_if_t interface;
    esp_ble_gattc_cb_param_t param;
} lotus_gatt_event_t;
static QueueHandle_t gatt_events;
static volatile bool event_queue_overflow;
static volatile bool scan_stop_received;
static volatile bool scan_stop_failed;
static bool enabled;
static bool trial_mode;
static int64_t trial_ends_ms;
static int64_t last_diagnostics_ms;
static esp_gatt_if_t client_if=ESP_GATT_IF_NONE;
static esp_mqtt_client_handle_t mqtt;
static char base_topic[32];
/* Retained MQTT availability is published outside the MQTT event callback.
 * OFF = unavailable in HA, even if the discovery config is retained. */
static esp_mqtt_client_handle_t availability_client;
static char availability_topic[112];
static volatile bool availability_pending;
static int64_t availability_retry_ms;
/* Publish MQTT Discovery from the r4sGate main loop, NOT the MQTT callback.
 * Static scratch buffers protect the 4.4 kB ESP32 main-task stack. */
static unsigned discovery_cursor;
static int64_t next_discovery_ms;
static char discovery_topic[160];
static char discovery_json[1000];
static char discovery_cmd[100];
static uint8_t bt_type[LOTUS_MAX_LIGHTS];
static bool seen[LOTUS_MAX_LIGHTS];
/* PASSIVE advertising observations: available even if Lotus control is OFF.
   Do not allocate in the Bluedroid GAP callback or create a second scanner. */
typedef struct {
    uint8_t mac[6];
    char name[18];
    int rssi;
    int64_t at_ms;
    bool used;
} lotus_candidate_t;
static lotus_candidate_t candidates[LOTUS_SCAN_CANDIDATES];
static portMUX_TYPE candidate_mux=portMUX_INITIALIZER_UNLOCKED;
static uint8_t retry_count[LOTUS_MAX_LIGHTS];
static uint32_t queued_writes[LOTUS_MAX_LIGHTS];
static int64_t last_write_ms[LOTUS_MAX_LIGHTS];
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
    for(int step=0;step<(int)LOTUS_MAX_LIGHTS;step++) {
        int i=((int)manager.round_robin+step)%(int)LOTUS_MAX_LIGHTS;
        if(any_pending(i) && seen[i] && now-last_seen[i]<LOTUS_ADV_MAX_AGE_MS && now>=retry_after[i])return i;
    }
    return -1;
}
static void reset_connection(void) {
    link_state=L_IDLE; active=-1; connection_id=0;
    start_handle=end_handle=write_handle=0; link_since=0; last_tx=0;
}
static void mark_retry(int slot) {
    if(slot<0 || slot>=(int)LOTUS_MAX_LIGHTS) return;
    unsigned count=retry_count[slot];
    if(count<5) retry_count[slot]++;
    uint32_t pause=LOTUS_RETRY_MIN_MS;
    for(unsigned i=0;i<count && pause<LOTUS_RETRY_MAX_MS/2;i++) pause*=2;
    if(pause>LOTUS_RETRY_MAX_MS)pause=LOTUS_RETRY_MAX_MS;
    retry_after[slot]=now_ms()+pause;
    lotus_diag_event("BLE retry scheduled",slot+1,(int)pause);
}
static void fail_connection(void) {
    mark_retry(active);
    reset_connection();
}
static void begin_open(void) {
    if(active<0 || active>=(int)LOTUS_MAX_LIGHTS || client_if==ESP_GATT_IF_NONE) {reset_connection();return;}
    lotus_diag_event("GATT open requested",active+1,(int)bt_type[active]);
    esp_err_t err=esp_ble_gattc_open(client_if,manager.light[active].mac,
                                      (esp_ble_addr_type_t)bt_type[active],true);
    if(err!=ESP_OK) {
        lotus_diag_event("GATT open refused",(int)err,0);
        ESP_LOGW(TAG,"BLE open refused: %s",esp_err_to_name(err));fail_connection();return;
    }
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

bool lotus_enabled(void) { return enabled; }
bool lotus_owns_gattc_if(esp_gatt_if_t gattc_if) {
    return enabled && client_if != ESP_GATT_IF_NONE &&
           gattc_if != ESP_GATT_IF_NONE && gattc_if == client_if;
}

void lotus_init(void) {
    lotus_diag_boot();
    lotus_diag_event("Init begin",0,0);
    enabled=false;
    trial_mode=false;
    trial_ends_ms=0;
    event_queue_overflow=false;
    scan_stop_received=false;
    scan_stop_failed=false;
    discovery_cursor=LOTUS_MAX_LIGHTS;
    next_discovery_ms=0;
    memset(&manager,0,sizeof(manager));
    memset(retry_count,0,sizeof(retry_count));
    memset(retry_after,0,sizeof(retry_after));
    memset(queued_writes,0,sizeof(queued_writes));
    memset(last_write_ms,0,sizeof(last_write_ms));
    manager.count=LOTUS_MAX_LIGHTS;
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        manager.light[i].r=manager.light[i].g=manager.light[i].b=255;
        manager.light[i].brightness=100;
        snprintf(manager.light[i].name,sizeof(manager.light[i].name),"Lotus %d",i+1);
    }
    nvs_handle_t h;
    if(nvs_open("lotus",NVS_READWRITE,&h)==ESP_OK){
        /* Version 2 fail-safe: an existing v1 MAC does NOT enable BLE traffic.
         * A one-time trial flag is consumed before any BLE connection can open,
         * so a crash/reboot during the trial always comes back disabled. */
        uint8_t always=0, trial=0, initialized=0;
        nvs_get_u8(h,"mode",&always);
        nvs_get_u8(h,"trial",&trial);
        /* Fail safe on an unexpected reboot in persistent mode: never
         * boot-loop the ESP32 and interrupt the four PVVX thermometers. */
        esp_reset_reason_t reason=esp_reset_reason();
        bool unsafe_reset=(reason==ESP_RST_PANIC || reason==ESP_RST_WDT ||
                           reason==ESP_RST_TASK_WDT || reason==ESP_RST_INT_WDT ||
                           reason==ESP_RST_BROWNOUT);
        if(always && unsafe_reset) {
            lotus_diag_event("Unsafe reset, Lotus OFF",(int)reason,0);
            always=0;trial=0;
            nvs_set_u8(h,"mode",0);
            nvs_set_u8(h,"trial",0);
            nvs_commit(h);
        }
        if(trial){
            lotus_diag_event("Trial flag found",1,0);
            nvs_set_u8(h,"trial",0);
            esp_err_t disarm_result=nvs_commit(h);
            lotus_diag_event("Trial disarm commit",(int)disarm_result,0);
            if(disarm_result==ESP_OK) {
                enabled=true;
                trial_mode=true;
            }
            trial_ends_ms=now_ms()+LOTUS_TRIAL_MS;
        }else if(always==1){
            enabled=true;
            lotus_diag_event("Always-on selected",1,0);
        }
        nvs_get_u8(h,"init",&initialized);
        if(!initialized){
            /* First strip was verified in nRF Connect by the owner. */
            nvs_set_str(h,"mac0","BE:37:FA:00:7F:F2");
            nvs_set_u8(h,"init",1);
            nvs_commit(h);
        }
        for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
            char key[8]="mac0",s[24]={0};size_t len=sizeof(s);uint8_t mac[6];
            key[3]=(char)('0'+i);
            if(nvs_get_str(h,key,s,&len)==ESP_OK && lotus_parse_mac(s,mac)){
                memcpy(manager.light[i].mac,mac,6);
                manager.light[i].enabled=true;
            }
            memcpy(key,"nm0",4);key[2]=(char)('0'+i);
            char label[32]={0};size_t label_len=sizeof(label);
            if(nvs_get_str(h,key,label,&label_len)==ESP_OK && label[0])
                snprintf(manager.light[i].name,sizeof(manager.light[i].name),"%s",label);
        }
        nvs_close(h);
    }
    /* No extra FreeRTOS allocations at all while Lotus is disabled. */
    if(enabled){
        lotus_diag_event("BLE alloc begin",0,0);
        if(!mutex)mutex=xSemaphoreCreateMutex();
        if(!gatt_events)gatt_events=xQueueCreate(LOTUS_EVENT_QUEUE_LEN,sizeof(lotus_gatt_event_t));
        lotus_diag_event("BLE alloc result",mutex!=NULL,gatt_events!=NULL);
        if(!mutex || !gatt_events){
            ESP_LOGE(TAG,"Lotus allocation failed; BLE control disabled");
            lotus_diag_event("BLE alloc FAILED",0,0);
            enabled=false;
        }
    }
    lotus_diag_event("Init complete",enabled,trial_mode);
    ESP_LOGI(TAG,"Lotus manager ready; enabled=%d trial=%d; 5 slots; shared passive scan",enabled,trial_mode);
}
void lotus_scan_candidate(const uint8_t mac[6], const char *adv_name, int rssi) {
    if(!mac || !adv_name) return;
    /* Record only ELK controllers. Other manufacturers use different GATT
       formats; sightings are shown, but never auto-enabled or connected. */
    if(strncmp(adv_name,"ELK-BLEDOM",10)!=0 &&
       strncmp(adv_name,"ELK-BLEDDM",10)!=0) return;
    int64_t stamp=now_ms();
    portENTER_CRITICAL(&candidate_mux);
    int target=-1, oldest=0;
    for(int i=0;i<LOTUS_SCAN_CANDIDATES;i++) {
        if(candidates[i].used && !memcmp(candidates[i].mac,mac,6)) {target=i;break;}
        if(!candidates[i].used && target<0)target=i;
        if(candidates[i].at_ms<candidates[oldest].at_ms) oldest=i;
    }
    if(target<0) target=oldest;
    lotus_candidate_t *c=&candidates[target];
    memcpy(c->mac,mac,6);
    /* Constant fixed strings, no string manipulation in the critical part. */
    const char *type=strncmp(adv_name,"ELK-BLEDDM",10)==0?"ELK-BLEDDM":"ELK-BLEDOM";
    memcpy(c->name,type,11);
    c->name[10]=0;
    c->rssi=rssi;c->at_ms=stamp;c->used=true;
    portEXIT_CRITICAL(&candidate_mux);
}
void lotus_scan_seen(const uint8_t mac[6], esp_ble_addr_type_t type, int rssi){
    if(!enabled || !mac || !mutex)return;
    /* Called for every BLE advertisement on the Bluedroid task: NEVER wait. */
    if(xSemaphoreTake(mutex,0)!=pdTRUE)return;
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        if(manager.light[i].enabled && !memcmp(mac,manager.light[i].mac,6)){
            bt_type[i]=(uint8_t)type;seen[i]=true;
            last_seen[i]=now_ms();rssi_values[i]=rssi;
            break;
        }
    }
    give();
}
bool lotus_busy(void) {return enabled && link_state!=L_IDLE;}

static void lotus_process_gatt_event(esp_gattc_cb_event_t event,esp_gatt_if_t interface,
                                     esp_ble_gattc_cb_param_t *p);

void lotus_tick(bool scanning,bool *scan_stop_req,bool setting_scan,
                bool ota_active,bool other_ble_busy) {
    if(!ota_active && availability_pending && availability_client &&
       now_ms()>=availability_retry_ms) {
        esp_mqtt_client_handle_t c=availability_client;
        int rc=esp_mqtt_client_publish(c,availability_topic,
                                       enabled?"online":"offline",0,1,1);
        availability_pending=rc<0;
        availability_retry_ms=now_ms()+3000;
        lotus_diag_event("MQTT availability",enabled,rc>=0);
    }
    if(!enabled || !mutex || !gatt_events || ota_active)return;
    take();
    /* BLE events are applied only here, outside Bluedroid callback context. */
    if(event_queue_overflow){
        ESP_LOGE(TAG,"GATT event queue overflow: stopping Lotus until reboot");
        lotus_diag_event("QUEUE OVERFLOW - OFF",0,0);
        enabled=false;
        restart_at=now_ms()+1500;
        restart_requested=true;
        give();
        /* Persistent mode must not create a crash loop. */
        nvs_handle_t h;
        if(nvs_open("lotus",NVS_READWRITE,&h)==ESP_OK){
            nvs_set_u8(h,"mode",0);
            nvs_set_u8(h,"trial",0);
            nvs_commit(h);
            nvs_close(h);
        }
        return;
    }
    lotus_gatt_event_t ev;
    int budget=LOTUS_EVENT_QUEUE_LEN;
    while(budget-- > 0 && xQueueReceive(gatt_events,&ev,0)==pdTRUE){
        lotus_process_gatt_event(ev.event,ev.interface,&ev.param);
    }
    if(scan_stop_failed){
        scan_stop_failed=false;
        scan_stop_received=false;
        lotus_diag_event("Scan stop failed - retry",active+1,0);
        if(link_state==L_WAIT_SCAN_STOP) fail_connection();
    }
    if(scan_stop_received){
        scan_stop_received=false;
        if(link_state==L_WAIT_SCAN_STOP) begin_open();
    }
    if(now_ms()-last_diagnostics_ms>=10000){
        last_diagnostics_ms=now_ms();
        lotus_diag_event("Lotus heap / stack bytes",(int)esp_get_free_heap_size(),
                         (int)uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG,"status state=%d heap=%"PRIu32" trial=%d remaining=%lld s",(int)link_state,
                 (uint32_t)esp_get_free_heap_size(),trial_mode,
                 (long long)(trial_mode?(trial_ends_ms-now_ms())/1000:0));
    }
    int64_t now=now_ms();
    if(link_state==L_IDLE && !other_ble_busy && client_if!=ESP_GATT_IF_NONE) {
        int candidate=next_pending();
        if(candidate>=0 && !setting_scan && !*scan_stop_req){
            active=candidate;
            if(scanning) {
                /* Set ownership BEFORE requesting stop.  The Bluedroid GAP
                 * callback can arrive before esp_ble_gap_stop_scanning()
                 * returns; the old order dropped this completion and left
                 * Lotus stuck in L_WAIT_SCAN_STOP until BLE TIMEOUT. */
                link_state=L_WAIT_SCAN_STOP;
                link_since=now;
                scan_stop_received=false;
                scan_stop_failed=false;
                *scan_stop_req=true;
                lotus_diag_event("Scan stop requested",active+1,1);
                esp_err_t stop_err=esp_ble_gap_stop_scanning();
                lotus_diag_event("Scan stop API result",(int)stop_err,active+1);
                if(stop_err!=ESP_OK){
                    /* No completion is owed after an API-level rejection. */
                    *scan_stop_req=false;
                    scan_stop_received=false;
                    scan_stop_failed=false;
                    lotus_diag_event("Scan stop API rejected",(int)stop_err,active+1);
                    fail_connection();
                }
            } else {
                lotus_diag_event("No scan, opening GATT",active+1,0);
                begin_open();
            }
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
                    lotus_diag_event("GATT write queued",active+1,(int)action);
                    lotus_manager_ack_action(&manager,(size_t)active,action);
                    publish_pending[active]=true;have_state[active]=true;
                    queued_writes[active]++;
                    last_write_ms[active]=now;
                    last_tx=now;
                }else{
                    lotus_diag_event("GATT write refused",(int)err,active+1);
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
        lotus_diag_event("BLE TIMEOUT",(int)link_state,active+1);
        ESP_LOGW(TAG,"BLE timeout state=%d",(int)link_state);
        if(link_state==L_WAIT_SCAN_STOP){
            /* Release only the Lotus-owned scan-stop request on timeout;
             * preserve the existing scanner and never open GATT without
             * an acknowledged successful GAP stop event. */
            *scan_stop_req=false;
            scan_stop_received=false;
            scan_stop_failed=false;
            lotus_diag_event("Stop event missing",(int)scanning,active+1);
        }
        if(link_state==L_DISCOVERING||link_state==L_READY) {
            mark_retry(active);
            close_connection();
        } else if(link_state==L_OPENING) {
            /* Legacy gattc_open has no compatible cancellation on IDF 5.4;
               discard late OPEN_EVT and close any resulting connection. */
            fail_connection();
        } else fail_connection();
    }
    /* Stage ONE MQTT publish under lock, then publish after releasing it.
     * Never block the main loop or MQTT worker while holding the BLE mutex. */
    /* Stage one discovery for publication after the BLE mutex is released.
     * This avoids synchronous cJSON + MQTT work in MQTT_EVENT_CONNECTED. */
    esp_mqtt_client_handle_t discovery_client=NULL;
    int discovery_slot=-1;
    if (mqtt && discovery_cursor < LOTUS_MAX_LIGHTS && now >= next_discovery_ms) {
        unsigned i=discovery_cursor++;
        if (manager.light[i].enabled) {
            char mac[13];
            if (lotus_ha_discovery(&manager,i,base_topic,
                                   discovery_topic,sizeof(discovery_topic),
                                   discovery_json,sizeof(discovery_json))) {
                lotus_mac_compact(manager.light[i].mac,mac);
                snprintf(discovery_cmd,sizeof(discovery_cmd),"%s/lotus/%s/set",base_topic,mac);
                discovery_client=mqtt;
                discovery_slot=(int)i;
                if (have_state[i]) publish_pending[i]=true;
            } else {
                lotus_diag_event("MQTT discovery encoding fail",i+1,0);
            }
        }
    }
    char topic[96]={0},value[220]={0};
    esp_mqtt_client_handle_t client=mqtt;
    int publish_slot=-1;
    if(client && base_topic[0]) {
        for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++) {
            if(publish_pending[i] && !any_pending(i)){
                char mac[13];
                lotus_mac_compact(manager.light[i].mac,mac);
                snprintf(topic,sizeof(topic),"%s/lotus/%s/state",base_topic,mac);
                if(lotus_ha_state(&manager,i,value,sizeof(value))) {
                    publish_pending[i]=false;
                    publish_slot=i;
                }
                break;
            }
        }
    }
    give();
    if (discovery_slot>=0 && discovery_client) {
        lotus_diag_event("MQTT discovery begin",discovery_slot+1,0);
        int result=esp_mqtt_client_publish(discovery_client,discovery_topic,discovery_json,0,1,1);
        int sub=result>=0?esp_mqtt_client_subscribe(discovery_client,discovery_cmd,0):-1;
        lotus_diag_event("MQTT discovery result",result>=0,sub>=0);
        if (result<0 || sub<0) {
            take();
            if (mqtt==discovery_client) {
                discovery_cursor=(unsigned)discovery_slot;
                next_discovery_ms=now_ms()+3000;
            }
            give();
        }
    }
    if(publish_slot>=0 && esp_mqtt_client_publish(client,topic,value,0,1,1)<0) {
        take();
        if(enabled && mqtt==client)publish_pending[publish_slot]=true;
        give();
    }
}
bool lotus_scan_stopped(bool success) {
    if(!enabled)return false;
    /* Record even a stray/early completion. a=success, b=link state. */
    lotus_diag_event("GAP scan-stop callback",success?1:0,(int)link_state);
    if(link_state!=L_WAIT_SCAN_STOP)return false;
    /* Called from GAP callback: no blocking mutex or GATT calls here. */
    if(success) scan_stop_received=true;
    else scan_stop_failed=true;
    return true;
}

void lotus_gattc_event(esp_gattc_cb_event_t event,esp_gatt_if_t interface,
                       esp_ble_gattc_cb_param_t *p){
    if(!enabled || !p) return;
    if(event==ESP_GATTC_REG_EVT){
        if(p->reg.app_id==LOTUS_GATTC_APP_ID){
            lotus_diag_event("GATT register event",(int)p->reg.status,(int)interface);
            if(p->reg.status==ESP_GATT_OK){
                client_if=interface;ESP_LOGI(TAG,"GATTC registered: %u",(unsigned)interface);
            }
        }
        return;
    }
    if(client_if==ESP_GATT_IF_NONE || interface!=client_if || !gatt_events)return;
    lotus_gatt_event_t ev={.event=event,.interface=interface,.param=*p};
    if(xQueueSend(gatt_events,&ev,0)!=pdTRUE){
        lotus_diag_event("BLE event queue full",(int)event,0);
        event_queue_overflow=true;
    }
}

static void lotus_process_gatt_event(esp_gattc_cb_event_t event,esp_gatt_if_t interface,
                                     esp_ble_gattc_cb_param_t *p){
    switch(event){
    case ESP_GATTC_OPEN_EVT:
        if(link_state!=L_OPENING || active<0 || memcmp(p->open.remote_bda,manager.light[active].mac,6)){
            if(p->open.status==ESP_GATT_OK)esp_ble_gattc_close(client_if,p->open.conn_id);
            break;
        }
        if(p->open.status!=ESP_GATT_OK){
            lotus_diag_event("GATT open failed",(int)p->open.status,0);
            ESP_LOGW(TAG,"Open failed %d",p->open.status);fail_connection();break;
        }
        lotus_diag_event("GATT open OK",active+1,(int)p->open.conn_id);
        retry_count[active]=0;retry_after[active]=0;
        connection_id=p->open.conn_id;link_state=L_DISCOVERING;link_since=now_ms();
        lotus_diag_event("Discover service start",(int)connection_id,0);
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
                lotus_diag_event("FFF3 ready",active+1,rssi_values[active]);
                ESP_LOGI(TAG,"FFF3 found, slot=%d RSSI=%d",active+1,rssi_values[active]);
            }else{
                lotus_diag_event("FFF3 NOT FOUND",(int)p->search_cmpl.status,0);
                ESP_LOGW(TAG,"FFF0/FFF3 write-no-response not found");
                mark_retry(active);
                close_connection();
            }
        }
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        if((link_state==L_READY||link_state==L_CLOSING) && p->write.conn_id==connection_id && p->write.status!=ESP_GATT_OK){
            lotus_diag_event("GATT write event error",(int)p->write.status,0);
            ESP_LOGW(TAG,"BLE write event status=%u",(unsigned)p->write.status);
        }
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        if(active>=0 && !memcmp(p->disconnect.remote_bda,manager.light[active].mac,6)){
            lotus_diag_event("GATT disconnected",(int)p->disconnect.reason,active+1);
            if(p->disconnect.reason!=0x16 && any_pending(active))mark_retry(active);
            reset_connection();
        }
        break;
    default:break;
    }
}

void lotus_mqtt_connected(esp_mqtt_client_handle_t client,const char *prefix){
    if(!client || !prefix)return;
    availability_client=client;
    snprintf(availability_topic,sizeof(availability_topic),"%s/lotus/availability",prefix);
    availability_pending=true;
    availability_retry_ms=0;
    if(!enabled)return;
    /* Publish/subscribe belongs in lotus_tick(), never the MQTT callback.
     * This also leaves clear RTC breadcrumbs at the next main-loop tick. */
    take();
    mqtt=client;
    snprintf(base_topic,sizeof(base_topic),"%s",prefix);
    discovery_cursor=0;
    next_discovery_ms=0;
    give();
    lotus_diag_event("MQTT session queued",0,0);
}
void lotus_mqtt_disconnected(void){
    availability_client=NULL;
    availability_pending=false;
    take();mqtt=NULL;give();
}
bool lotus_mqtt_message(const char *topic,int topic_len,const char *payload,int payload_len,const char *prefix){
    if(!enabled || !topic||!payload||topic_len<=0||payload_len<=0||payload_len>1024||!prefix)return false;
    take();
    bool handled=lotus_ha_parse_command(&manager,prefix,topic,(size_t)topic_len,
                                         payload,(size_t)payload_len);
    give();
    if(handled)lotus_diag_event("MQTT light command",1,payload_len);
    return handled;
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
/* Escape labels before reflecting UTF-8 user names in HTML attributes. */
static void html_escape(const char *src,char *dst,size_t cap) {
    size_t pos=0;
    while(*src && pos+7<cap) {
        const char *replace=NULL;
        switch(*src) {
            case '&':replace="&amp;";break;
            case '<':replace="&lt;";break;
            case '>':replace="&gt;";break;
            case '"':replace="&quot;";break;
            case '\'':replace="&#39;";break;
        }
        if(replace) {size_t n=strlen(replace);memcpy(dst+pos,replace,n);pos+=n;}
        else {dst[pos++]=*src;}
        src++;
    }
    dst[pos]=0;
}
static bool valid_label(const char *s) {
    size_t n=strlen(s);
    if(n>=sizeof(manager.light[0].name))return false;
    for(size_t i=0;i<n;i++)if((unsigned char)s[i]<0x20 || s[i]==0x7f)return false;
    return true;
}
static void copy_candidates(lotus_candidate_t out[LOTUS_SCAN_CANDIDATES]) {
    portENTER_CRITICAL(&candidate_mux);
    memcpy(out,candidates,sizeof(candidates));
    portEXIT_CRITICAL(&candidate_mux);
}
esp_err_t lotus_http_get(httpd_req_t *req){
    char query[64]={0};
    if(httpd_req_get_url_query_str(req,query,sizeof(query))==ESP_OK && strstr(query,"log=1")) {
        httpd_resp_set_type(req,"text/plain; charset=utf-8");
        httpd_resp_set_hdr(req,"Cache-Control","no-store, no-cache, must-revalidate");
        lotus_diag_render(req,false);
        return httpd_resp_sendstr_chunk(req,NULL);
    }
    char macs[LOTUS_MAX_LIGHTS][18];
    char names[LOTUS_MAX_LIGHTS][32];
    uint32_t writes[LOTUS_MAX_LIGHTS];
    uint8_t reds[LOTUS_MAX_LIGHTS],greens[LOTUS_MAX_LIGHTS],blues[LOTUS_MAX_LIGHTS],levels[LOTUS_MAX_LIGHTS];
    int64_t wait_ms[LOTUS_MAX_LIGHTS];
    bool pending[LOTUS_MAX_LIGHTS];
    int rssis[LOTUS_MAX_LIGHTS];
    int64_t ages[LOTUS_MAX_LIGHTS];
    int64_t tm=now_ms();
    take();
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        mac_text(i,macs[i]);
        snprintf(names[i],sizeof(names[i]),"%s",manager.light[i].name);
        reds[i]=manager.light[i].r; greens[i]=manager.light[i].g;
        blues[i]=manager.light[i].b; levels[i]=manager.light[i].brightness;
        writes[i]=queued_writes[i];
        wait_ms[i]=retry_after[i]>tm?retry_after[i]-tm:0;
        pending[i]=any_pending(i);
        rssis[i]=rssi_values[i];
        ages[i]=seen[i]?((tm-last_seen[i])/1000):-1;
    }
    bool active=enabled,is_trial=trial_mode;
    int64_t remaining=trial_mode?(trial_ends_ms-tm)/1000:0;
    give();
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store, no-cache, must-revalidate");
    httpd_resp_sendstr_chunk(req,"<!doctype html><html lang='ru'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Lotus Lantern v7</title><style>body{font:16px system-ui;background:#161b22;color:#e6edf3;max-width:790px;margin:25px auto;padding:12px}input,select{font-size:1rem;padding:9px;background:#fff;color:#111;border:0;border-radius:7px;max-width:230px}input[type=color]{height:46px;width:75px}input[type=range]{width:185px}label{display:block;margin:13px 0}button{background:#238636;color:#fff;border:0;border-radius:7px;padding:10px 16px;margin:4px;cursor:pointer}button.secondary{background:#36415a}a{color:#58a6ff}.warn{color:#ffb86c}.card{border:1px solid #36415a;border-radius:12px;margin:13px 0;padding:14px}.detail{color:#a6b3c3;font-size:13px}h3{margin:4px 0}</style></head><body><h1>Lotus Lantern v7</h1><p>До пяти независимых ELK-BLEDOM. Датчики ATC/PVVX работают через существующий BLE-сканер. Не проверяйте неизвестные модели без подтверждения протокола.</p>");
    char status[240];
    snprintf(status,sizeof(status),"<p><b>Status: %s</b>%s%lld%s. Free heap: %"PRIu32" bytes.</p>",
             active?(is_trial?"TRIAL":"ENABLED"):"DISABLED",
             is_trial?"; auto-restart in ":"", (long long)(is_trial?(remaining>0?remaining:0):0),
             is_trial?" seconds":"",(uint32_t)esp_get_free_heap_size());
    httpd_resp_sendstr_chunk(req,status);
    httpd_resp_sendstr_chunk(req,"<form action='/lotus/save' method='POST'>");
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        static char b[768]; /* HTTP server has a limited task stack. */
        char safe[160];
        html_escape(names[i],safe,sizeof(safe));
        snprintf(b,sizeof(b),"<div class='card'><h3>Лента %d</h3><label>Название: <input name='nm%d' maxlength='31' value=\"%s\"></label><label>MAC: <input id='mac%d' name='mac%d' maxlength='17' placeholder='BE:37:FA:00:7F:F2' value='%s'></label><span class='detail'>RSSI %d dBm; последнее обнаружение %lld с назад</span></div>",
                 i+1,i,safe,i,i,macs[i],rssis[i],(long long)ages[i]);
        httpd_resp_sendstr_chunk(req,b);
    }
    httpd_resp_sendstr_chunk(req,"<p>Unknown ELK-BLEDDM protocol has not been verified. Leave its MAC empty.</p><label>Mode: <select name='mode'>");
    httpd_resp_sendstr_chunk(req,active ? (is_trial ?
            "<option value='off'>OFF (safest)</option><option value='trial' selected>TEST for 2 minutes</option><option value='on'>ALWAYS ON</option>" :
            "<option value='off'>OFF (safest)</option><option value='trial'>TEST for 2 minutes</option><option value='on' selected>ALWAYS ON</option>") :
            "<option value='off' selected>OFF (safest)</option><option value='trial'>TEST for 2 minutes</option><option value='on'>ALWAYS ON</option>");
    httpd_resp_sendstr_chunk(req,"</select></label><p class='warn'>При TEST ESP32 автоматически вернётся в OFF после 2 минут. После Panic в ALWAYS ON модуль Lotus отключится при следующем запуске.</p><button type='submit'>Сохранить настройки и перезагрузить</button></form>");
    httpd_resp_sendstr_chunk(req,"<h2>Обнаруженные контроллеры ELK</h2><p class='detail'>Нажмите «Добавить», затем сохраните настройки выше. ELK-BLEDDM может использовать другой протокол и здесь не проверен.</p>");
    lotus_candidate_t recent[LOTUS_SCAN_CANDIDATES];copy_candidates(recent);
    int shown=0;
    for(int i=0;i<LOTUS_SCAN_CANDIDATES;i++){
        if(!recent[i].used || tm-recent[i].at_ms>120000)continue;
        const uint8_t *m=recent[i].mac;
        char b[320];
        snprintf(b,sizeof(b),"<div class='card'><b>%s</b> — %02X:%02X:%02X:%02X:%02X:%02X <span class='detail'>(%d dBm)</span><button class='secondary' type='button' onclick=\"addMac('%02X:%02X:%02X:%02X:%02X:%02X')\">Добавить</button></div>",
           recent[i].name,m[0],m[1],m[2],m[3],m[4],m[5],recent[i].rssi,m[0],m[1],m[2],m[3],m[4],m[5]);
        httpd_resp_sendstr_chunk(req,b);shown++;
    }
    if(!shown)httpd_resp_sendstr_chunk(req,"<p class='detail'>Пока не обнаружены (проверьте, что BLE Monitor сканирует).</p>");
    httpd_resp_sendstr_chunk(req,"<script>function addMac(mac){for(let i=0;i<5;i++){let e=document.getElementById('mac'+i);if(e&&(!e.value||e.value.toUpperCase()===mac.toUpperCase())){e.value=mac;e.focus();return}}alert('Все пять слотов заняты')}</script>");
    httpd_resp_sendstr_chunk(req,"<h2>Управление лентами</h2><p class='detail'>Команды доступны только в TEST / ALWAYS ON. Счётчик показывает поставленные в очередь BLE-записи, а не подтверждение от ленты.</p>");
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        if(!macs[i][0])continue;
        static char b[960];
        char name_safe[160];
        html_escape(names[i],name_safe,sizeof(name_safe));
        snprintf(b,sizeof(b),"<div class='card'><h3>%s</h3><p class='detail'>%s | %s | записей в BLE: %"PRIu32" | ожидание: %lld с</p><form method='post' action='/lotus/control'><input type='hidden' name='slot' value='%d'><button name='action' value='on'>Включить</button><button name='action' value='off' class='secondary'>Выключить</button><p><label>Цвет <input type='color' name='color' value='#%02X%02X%02X'><button name='action' value='color'>Применить RGB</button></label></p><p><label>Яркость <input type='range' min='1' max='100' name='brightness' value='%u'><button name='action' value='brightness'>Применить</button></label></p></form></div>",
            name_safe,macs[i],pending[i]?"команды ожидают":"очередь пуста",writes[i],(long long)(wait_ms[i]/1000),i,reds[i],greens[i],blues[i],levels[i]);
        httpd_resp_sendstr_chunk(req,b);
    }
    lotus_diag_render(req,true);
    httpd_resp_sendstr_chunk(req,"<p><a href='/'>Back to r4sGate</a></p></body></html>");
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
    if(req->content_len<=0 || req->content_len>LOTUS_MAX_WEB_FORM){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid form size");return ESP_FAIL;}
    char form[LOTUS_MAX_WEB_FORM+1]={0};int pos=0;
    while(pos<req->content_len){
        int n=httpd_req_recv(req,form+pos,req->content_len-pos);
        if(n<=0){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete form");return ESP_FAIL;}
        pos+=n;
    }
    char macs[LOTUS_MAX_LIGHTS][18]={{0}};
    char names[LOTUS_MAX_LIGHTS][32]={{0}};
    bool has_name[LOTUS_MAX_LIGHTS]={0};
    char mode[8]={0};
    bool mode_field=false;
    bool fields[LOTUS_MAX_LIGHTS]={0};
    char *save=NULL;
    for(char *part=strtok_r(form,"&",&save);part;part=strtok_r(NULL,"&",&save)){
        if(!strncmp(part,"mode=",5)){
            decode_form_value(part+5,strlen(part+5),mode,sizeof(mode));
            mode_field=true;
            continue;
        }
        if(!strncmp(part,"nm",2) && part[2]>='0' && part[2]<='4' && part[3]=='=') {
            int index=part[2]-'0';
            decode_form_value(part+4,strlen(part+4),names[index],sizeof(names[index]));
            has_name[index]=true;
            continue;
        }
        if(strncmp(part,"mac",3)||part[3]<'0'||part[3]>'4'||part[4]!='=')continue;
        int slot=part[3]-'0';
        decode_form_value(part+5,strlen(part+5),macs[slot],sizeof(macs[slot]));
        fields[slot]=true;
    }
    if(!mode_field || (strcmp(mode,"off") && strcmp(mode,"trial") && strcmp(mode,"on"))){
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid mode");return ESP_FAIL;
    }
    for(int i=0;i<(int)LOTUS_MAX_LIGHTS;i++){
        if(!fields[i] || !has_name[i] || !valid_label(names[i]) ||
           (macs[i][0] && !lotus_parse_mac(macs[i],(uint8_t[6]){0}))){
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
        char key[8]="mac0";key[3]=(char)('0'+i);
        if(nvs_set_str(h,key,macs[i])!=ESP_OK)ret=ESP_FAIL;
        memcpy(key,"nm0",4);key[2]=(char)('0'+i);
        if(nvs_set_str(h,key,names[i])!=ESP_OK)ret=ESP_FAIL;
    }
    if(ret==ESP_OK && nvs_set_u8(h,"mode",!strcmp(mode,"on")?1:0)!=ESP_OK)ret=ESP_FAIL;
    if(ret==ESP_OK && nvs_set_u8(h,"trial",!strcmp(mode,"trial")?1:0)!=ESP_OK)ret=ESP_FAIL;
    if(ret==ESP_OK)ret=nvs_commit(h);
    lotus_diag_event("WEB saved config",!strcmp(mode,"trial")?1:(!strcmp(mode,"on")?2:0),(int)ret);
    nvs_close(h);
    if(ret!=ESP_OK){httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"NVS write failed");return ESP_FAIL;}
    httpd_resp_set_status(req,"303 See Other");
    httpd_resp_set_hdr(req,"Location","/lotus");
    httpd_resp_send(req,NULL,0);
    restart_at=now_ms()+2000;restart_requested=true;
    return ESP_OK;
}
esp_err_t lotus_http_command(httpd_req_t *req) {
    /* No configuration/NVS changes: just queue a command for the main loop. */
    if(!enabled || !mutex){
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Lotus is OFF. Enable TEST or ALWAYS ON first.");
        return ESP_FAIL;
    }
    if(req->content_len<=0 || req->content_len>160){
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid command size");return ESP_FAIL;
    }
    char form[161]={0}, action[24]={0}, color[16]={0};
    int read=0,slot=-1,brightness=-1;
    while(read<req->content_len){
        int n=httpd_req_recv(req,form+read,req->content_len-read);
        if(n<=0){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete command");return ESP_FAIL;}
        read+=n;
    }
    char *save=NULL;
    for(char *part=strtok_r(form,"&",&save);part;part=strtok_r(NULL,"&",&save)){
        if(!strncmp(part,"slot=",5)){
            if(strlen(part+5)==1 && part[5]>='0' && part[5]<='4')slot=part[5]-'0';
        }else if(!strncmp(part,"action=",7)){
            decode_form_value(part+7,strlen(part+7),action,sizeof(action));
        }else if(!strncmp(part,"color=",6)){
            decode_form_value(part+6,strlen(part+6),color,sizeof(color));
        }else if(!strncmp(part,"brightness=",11)){
            char val[8]={0};decode_form_value(part+11,strlen(part+11),val,sizeof(val));
            if(strlen(val)<=3 && strspn(val,"0123456789")==strlen(val) && val[0])brightness=atoi(val);
        }
    }
    if(slot<0 || slot>=(int)LOTUS_MAX_LIGHTS){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid slot");return ESP_FAIL;}
    bool ok=false;
    take();
    if(manager.light[slot].enabled){
        if(!strcmp(action,"on"))ok=lotus_manager_power(&manager,slot,true);
        else if(!strcmp(action,"off"))ok=lotus_manager_power(&manager,slot,false);
        else if(!strcmp(action,"brightness") && brightness>=1 && brightness<=100){
            ok=lotus_manager_brightness(&manager,slot,(uint8_t)brightness);
            if(!manager.light[slot].power)lotus_manager_power(&manager,slot,true);
        } else if(!strcmp(action,"color") && color[0]=='#' && strlen(color)==7){
            int rgb[3];bool valid=true;
            for(int c=0;c<3;c++){
                int hi=fromhex(color[1+c*2]),lo=fromhex(color[2+c*2]);
                if(hi<0 || lo<0){valid=false;break;}rgb[c]=hi*16+lo;
            }
            if(valid){ok=lotus_manager_rgb(&manager,slot,(uint8_t)rgb[0],(uint8_t)rgb[1],(uint8_t)rgb[2]);
                if(!manager.light[slot].power)lotus_manager_power(&manager,slot,true);
            }
        }
    }
    give();
    if(!ok){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Unsupported command or empty slot");return ESP_FAIL;}
    lotus_diag_event("WEB light command",slot+1,0);
    httpd_resp_set_status(req,"303 See Other");
    httpd_resp_set_hdr(req,"Location","/lotus");
    return httpd_resp_send(req,NULL,0);
}
bool lotus_restart_due(void){
    /* Failed networks cannot leave test mode stuck ON forever. */
    bool due=(restart_requested && now_ms()>restart_at) ||
             (trial_mode && trial_ends_ms>0 && now_ms()>trial_ends_ms);
    if(due) lotus_diag_event("Planned ESP restart",trial_mode,0);
    return due;
}
