# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# The VS1053 driver is disabled (audio goes through the PCM5102 over I2S). Remove this
# filter to build it again.
list(FILTER app_sources EXCLUDE REGEX "/vs1053\\.c$")

idf_component_register(SRCS ${app_sources} PRIV_REQUIRES bt nvs_flash esp_driver_pcnt esp_driver_i2s)
