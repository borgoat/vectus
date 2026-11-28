set(requires vectus esp_http_client esp_event)
idf_build_get_property(target IDF_TARGET)
if(NOT target STREQUAL "linux")
    list(APPEND requires protocol_examples_common nvs_flash esp_netif)  # Wi-Fi on the board
endif()

idf_component_register(SRCS "test.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES ${requires})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
