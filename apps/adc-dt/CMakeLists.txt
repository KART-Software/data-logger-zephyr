cmake_minimum_required(VERSION 3.20.0)

# ADS868x ドライバはこのリポで保守する out-of-tree モジュール (zephyr-modules/ads868x)
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr-modules/ads868x)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(adc_dt)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_SOC_MIMX8ML8 app PRIVATE src/imx8mp_clocks.c)
