# out-of-tree 用の CMake。upstream では drivers/adc/CMakeLists.txt に
#   zephyr_library_sources_ifdef(CONFIG_ADC_ADS868X adc_ads868x.c)
# の 1 行を足すだけ (README 参照)。
if(CONFIG_ADC_ADS868X)
  zephyr_library_named(adc_ads868x)
  zephyr_library_sources(drivers/adc/adc_ads868x.c)
  # adc_context.h は in-tree ADC ドライバの私有ヘッダ
  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/adc)
  # ソースは upstream main の API 名 (SPI_OP_MODE_CONTROLLER 等)。v4.3.0 に無い名前を埋める
  zephyr_library_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/zephyr_v4_3.h)
endif()
