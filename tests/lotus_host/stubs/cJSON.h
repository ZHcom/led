#pragma once
#include <stddef.h>
typedef struct cJSON {struct cJSON *next,*prev,*child;int type;char *valuestring;int valueint;double valuedouble;char *string;} cJSON;
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddArrayToObject(cJSON*,const char*);
cJSON *cJSON_AddObjectToObject(cJSON*,const char*);
cJSON *cJSON_CreateString(const char*);
int cJSON_AddItemToArray(cJSON*,cJSON*);
cJSON *cJSON_AddStringToObject(cJSON*,const char*,const char*);
cJSON *cJSON_AddNumberToObject(cJSON*,const char*,double);
char *cJSON_PrintUnformatted(const cJSON*);
void cJSON_free(void*);
void cJSON_Delete(cJSON*);
cJSON *cJSON_ParseWithLength(const char*,size_t);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON*,const char*);
int cJSON_IsNumber(const cJSON*);
int cJSON_IsString(const cJSON*);
int cJSON_IsObject(const cJSON*);
