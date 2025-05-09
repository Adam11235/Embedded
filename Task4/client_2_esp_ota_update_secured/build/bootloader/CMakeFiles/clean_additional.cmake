# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "bootloader.bin"
  "bootloader.map"
  "config\\sdkconfig.cmake"
  "config\\sdkconfig.h"
  "esp-idf\\bootloader_support\\signature_verification_key.bin"
  "project_elf_src_esp32.c"
  "signature_verification_key.bin.S"
  )
endif()
