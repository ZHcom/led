#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef struct {int content_len;} httpd_req_t;
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
esp_err_t httpd_resp_set_type(httpd_req_t*,const char*);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t*,const char*);
esp_err_t httpd_resp_send_err(httpd_req_t*,int,const char*);
esp_err_t httpd_resp_set_status(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_send(httpd_req_t*,const char*,int);
int httpd_req_recv(httpd_req_t*,char*,size_t);
