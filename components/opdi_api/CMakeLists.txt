set(srcs opdi_api_ws.c opdi_api_static.c)

# Embed assets if desired (Phase2 optimization). For now we read from SPIFFS /spiffs/web.
# If the files exist at configure time we can choose to embed (disabled by default to allow editing without rebuild).
# set(embed_assets 0)
# if(embed_assets)
#   idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include" REQUIRES esp_http_server opdi_net EMBED_FILES ../../web/index.html ../../web/app.js ../../web/style.css)
# else()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include" REQUIRES esp_http_server opdi_net)
# endif()
