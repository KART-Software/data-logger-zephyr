cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(main_app)

# 土台: 起動とコンソール (main.c)、Linux との rpmsg (rpmsg_platform.c)、キープアライブ (keepalive.c)。
# 機能: CAN の中継 (can_gw.c)。ADC はファイルを分けてここに足す
target_sources(app PRIVATE
  src/main.c
  src/rpmsg_platform.c
  src/keepalive.c
  src/can_gw.c
)
