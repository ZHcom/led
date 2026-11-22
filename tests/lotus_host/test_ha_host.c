#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lotus_protocol.h"
#include "lotus_ha.h"
int main(void){
 lotus_manager_t m;lotus_manager_reset(&m);
 assert(lotus_manager_add(&m,"BE:37:FA:00:7F:F2"));
 char topic[200],json[1024];
 assert(lotus_ha_discovery(&m,0,"r4s0",topic,sizeof(topic),json,sizeof(json)));
 assert(strstr(topic,"homeassistant/light/") && strstr(json,"supported_color_modes") && strstr(json,"command_topic"));
 const char *cmd="{\"state\":\"ON\",\"brightness\":51,\"color\":{\"r\":123,\"g\":20,\"b\":240}}";
 assert(lotus_ha_parse_command(&m,"r4s0","r4s0/lotus/be37fa007ff2/set",strlen("r4s0/lotus/be37fa007ff2/set"),cmd,strlen(cmd)));
 assert(m.light[0].pending_power && m.light[0].pending_rgb && m.light[0].pending_brightness);
 assert(m.light[0].brightness==20);
 assert(lotus_ha_state(&m,0,json,sizeof(json)));
 assert(strstr(json,"\"state\":\"ON\"") && strstr(json,"\"r\":123"));
 assert(!lotus_ha_parse_command(&m,"r4s0","r4s0/lotus/bad/set",strlen("r4s0/lotus/bad/set"),cmd,strlen(cmd)));
 puts("OK: MQTT discovery, JSON Light commands, RGB, brightness, state");return 0;
}
