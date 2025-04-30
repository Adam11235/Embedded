idf_component_register(SRCS "t2.c"
                    INCLUDE_DIRS "."
                    REQUIRES)

spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)