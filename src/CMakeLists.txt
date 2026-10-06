# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# The VS1053 driver and the MP3 header parser are disabled for now (PCM5102 + WAV
# only). Remove this filter to build them again.
list(FILTER app_sources EXCLUDE REGEX "/(vs1053|mp3_info)\\.c$")

idf_component_register(SRCS ${app_sources} PRIV_REQUIRES bt nvs_flash esp_driver_pcnt esp_driver_i2s)
