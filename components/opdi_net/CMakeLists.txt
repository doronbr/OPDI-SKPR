idf_component_register(
  SRCS "opdi_net.c" "opdi_net_hosted.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_wifi nvs_flash esp_event lwip mbedtls json
)
