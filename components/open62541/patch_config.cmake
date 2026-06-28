# Patch open62541 config.h to remove host-detected defines not available on ESP32/FreeRTOS
if(EXISTS "${CONFIG_H}")
    file(READ "${CONFIG_H}" config_content)
    string(REPLACE "#define UA_HAS_GETIFADDR 1" "/* #undef UA_HAS_GETIFADDR (not available on FreeRTOS/ESP32) */" config_content "${config_content}")
    file(WRITE "${CONFIG_H}" "${config_content}")
    message(STATUS "Patched ${CONFIG_H}: disabled UA_HAS_GETIFADDR")
endif()
