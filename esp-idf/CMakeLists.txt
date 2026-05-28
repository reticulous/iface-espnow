file(GLOB ESPNOW_SRCS "src/*.cpp")

idf_component_register(
    SRCS ${ESPNOW_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-net
        reticulous-core
        esp_wifi
        esp_event
)
