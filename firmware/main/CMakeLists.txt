idf_component_register(
    SRCS "main.c" "render.c" "display.c" "audio.c" "net.c" "pet.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_gpio
             esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls nvs_flash
             esp_partition esp_timer esp_driver_rmt
    EMBED_FILES "sprites.bin"
)
