idf_component_register(
    SRCS "hello_world_main.c"
    PRIV_REQUIRES esp_timer driver freertos log spi_flash
    INCLUDE_DIRS "."
)