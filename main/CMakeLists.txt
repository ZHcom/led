idf_component_register(SRCS "r4sGate.c" "lotus_protocol.c" "lotus_ha.c" "lotus_bridge.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt mqtt json nvs_flash esp_timer esp_http_server)
