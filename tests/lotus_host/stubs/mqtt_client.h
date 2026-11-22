#pragma once
typedef void *esp_mqtt_client_handle_t;
int esp_mqtt_client_publish(esp_mqtt_client_handle_t,const char*,const char*,int,int,int);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t,const char*,int);
