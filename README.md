# data-logger-imx8mm-cortex-m4

XPI-iMX8MM (Geniatech) の Cortex-M4 で動くデータロガーファームウェア (Zephyr RTOS)。
ESP32-S3 版 data-logger の後継。M4 は「CAN ゲートウェイ + ロガー」として、
ADS8688 (ADC)・スイッチ入力を 30Hz で CAN (MCP2518FD) に流し、
CAN 受信全量を rpmsg で Linux (kmm) へ転送する。

- 設計たたき台: kmm-yocto/local/carrier-board-design.md
- ボード側の受け入れ態勢 (remoteproc/DT): kmm-yocto リポジトリ
  (docs/imx8mm-xpi-bringup/10-cortex-m4.md)
- ロード方法: Linux remoteproc (/lib/firmware に zephyr.elf →
  /sys/class/remoteproc/remoteproc0 で start/stop)

## 構成

| dir | 内容 |
|---|---|
| firmware/ | west ワークスペースルート (zephyr/, modules/ は gitignore) |
| firmware/apps/ | manifest (west.yml) + アプリ群 |
| firmware/apps/hello-world/ | 動作確認 (printk → UART4 = XPI J64) |
