idf_component_register(SRCS "binarka.c"
                    INCLUDE_DIRS ".")
spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)