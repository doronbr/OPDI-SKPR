idf_component_register(SRCS "opdi_cam.c" INCLUDE_DIRS "include" REQUIRES nvs_flash esp_system driver esp_cam_sensor esp_sccb_intf esp32_p4_function_ev_board)

# Attempt to link esp_video if available (optional)
idf_build_get_property(build_components BUILD_COMPONENTS)
if("esp_video" IN_LIST build_components)
	target_link_libraries(${COMPONENT_LIB} PUBLIC esp_video)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE OPDI_CAM_HAVE_VIDEO=1)
endif()
