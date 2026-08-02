
idf_component_register(SRCS "main.c" "ws_connection_manager.c"
                       REQUIRES open62541 ethernet_helper nvs_flash esp_adc spi_flash espressif__mdns esp_http_server)

# Build static library, do not build test executables
option(BUILD_SHARED_LIBS OFF)
option(BUILD_TESTING OFF)

# ESP32 is little endian

# Unfortunately the library performs install and export. Would
# have been nice if devs made that an option like BUILD_SHARED_LIBS
# and BUILD_TESTING. Override install() and export() to do nothing
# instead.
function(install)
endfunction()

function(export)
endfunction()
