
set(USE_ENCRYPTION OFF)

set(ADDITIONAL_REQUIRES "")
if (USE_ENCRYPTION)
    set(ADDITIONAL_REQUIRES mbedtls)
endif()

idf_component_register(
        SRCS custom_getaddrinfo.c
        REQUIRES lwip vfs freertos ${ADDITIONAL_REQUIRES}
)

set_property(SOURCE ${CMAKE_BINARY_DIR}/open62541/open62541.c PROPERTY GENERATED 1)
set_source_files_properties(${CMAKE_BINARY_DIR}/open62541/open62541.c
    PROPERTIES COMPILE_FLAGS "-w")
target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_BINARY_DIR}/open62541/open62541.c)
target_include_directories(${COMPONENT_LIB} BEFORE PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/compat_includes
    ${CMAKE_BINARY_DIR}/open62541)

set(open62541_OPTIONS
    "-DUA_ARCHITECTURE=freertos-lwip"
    "-DFREERTOS_KERNEL_PATH=${CMAKE_CURRENT_SOURCE_DIR}/freertos_stub"
    "-DFREERTOS_PORT_PATH=${CMAKE_CURRENT_SOURCE_DIR}/freertos_stub"
    "-DUA_ENABLE_AMALGAMATION=ON"
    # REDUCED uses open62541's own curated minimal NS0 (Opc.Ua.NodeSet2.Reduced.xml
    # + per-feature *Minimal.xml nodesets), gated by the UA_ENABLE_* flags below.
    # This replaces the old FULL + hand-maintained nodes_blacklist.txt approach,
    # which broke whenever a blacklisted node was still transitively referenced.
    "-DUA_NAMESPACE_ZERO=REDUCED"
    "-DUA_ENABLE_DIAGNOSTICS=OFF"
    "-DUA_ENABLE_HISTORIZING=OFF"
    "-DUA_ENABLE_PUBSUB=OFF"
    "-DUA_ENABLE_DA=OFF"
    "-DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF"
    "-DUA_ENABLE_JSON_ENCODING=OFF"
    "-DUA_ENABLE_XML_ENCODING=OFF"
    "-DUA_ENABLE_STATUSCODE_DESCRIPTIONS=OFF"
    "-DUA_ENABLE_DISCOVERY=ON"
    # Generic server-side opc.ws:// wiring (src/server/ua_server_ws.c). We
    # provide our own "websocket" UA_ConnectionManager built on ESP-IDF's
    # esp_http_server instead of the POSIX-only, libwebsockets-based one
    # (UA_ENABLE_LWS) that open62541 ships internally.
    "-DUA_ENABLE_WEBSOCKET_TRANSPORT=ON"
    "-DUA_LOGLEVEL=300"
    "-DUA_ENABLE_NODESET_COMPILER_DESCRIPTIONS=OFF"
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    "-DLWIP_LIBRARIES=${CMAKE_BINARY_DIR}/esp-idf/lwip/liblwip.a"
    "-DCMAKE_PROJECT_INCLUDE_BEFORE=${CMAKE_CURRENT_SOURCE_DIR}/no_install.cmake"
)


if (USE_ENCRYPTION)
    set(open62541_OPTIONS ${open62541_OPTIONS} "-DUA_ENABLE_ENCRYPTION=ON")
endif()

set(open62541_CONFIG_H "${CMAKE_BINARY_DIR}/open62541/src_generated/open62541/config.h")

externalproject_add(open62541_build
                    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/repo
                    #GIT_REPOSITORY https://github.com/open62541/open62541.git
                    # Latest tested commit on master branch. Newer may work. You can also use 'master' directly as GIT_TAG value
                    #GIT_TAG 76a6cab604fb5309c91a8849183417da1383d3d9
                    BINARY_DIR "${CMAKE_BINARY_DIR}/open62541"
                    LIST_SEPARATOR "|"
                    CMAKE_ARGS  ${open62541_OPTIONS}
                        "-DLWIP_INCLUDE_DIRS=${IDF_PATH}/components/lwip/lwip/src/include|${IDF_PATH}/components/lwip/port/include|${IDF_PATH}/components/lwip/include"
                    PATCH_COMMAND ${CMAKE_COMMAND} -E echo "Patching config.h for ESP32 target"
                    BUILD_COMMAND
                        ${CMAKE_COMMAND} -DCONFIG_H=${open62541_CONFIG_H} -P
                            ${CMAKE_CURRENT_SOURCE_DIR}/patch_config.cmake
                        && cmake --build . --target open62541-amalgamation
                    INSTALL_COMMAND ""
                    )

add_dependencies(${COMPONENT_TARGET} open62541_build)
# Note: UA_getaddrinfo is internally mapped to lwip_getaddrinfo in open62541 v1.5.4.
# The custom_getaddrinfo workaround for ESP32 hostname resolution would need
# a linker-level override (--wrap=lwip_getaddrinfo) if still required.