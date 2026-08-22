# data-logger-zephyr

XPI-iMX8MM (Geniatech) の Cortex-M4 で動くデータロガーファームウェア (Zephyr RTOS)。
ESP32-S3 版 data-logger の後継。M4 は「CAN ゲートウェイ + ロガー」として、
ADS8688 (ADC)・スイッチ入力を 30Hz で CAN (MCP2518FD) に流し、
CAN 受信全量を rpmsg で Linux (kmm) へ転送する。

- 設計たたき台: kmm-yocto/local/carrier-board-design.md
- ボード側の受け入れ態勢 (remoteproc / DT / BL31 先住起動): kmm-yocto リポジトリ
  (docs/imx8mm-xpi-bringup/10-cortex-m4.md)
- ロード方法: Linux remoteproc (`/lib/firmware` に zephyr.elf →
  `/sys/class/remoteproc/remoteproc0` で start/stop)、または falcon SPL loadable
  + BL31 先住起動 (Linux は attach)

このリポジトリ自体が west workspace のルート (`zephyr/`・`modules/`・`.west/` は
gitignore、`west update` で取得)。

`patches/<module>/*.patch` は west モジュールへのローカルパッチ (bitbake の
SRC_URI パッチ相当。west に標準機構が無いため `west-container.sh` が冪等適用
する — `west update` 後も自動で当て直る)。現在は zephyr の `spi_mcux_ecspi`
修正 2 本 (GPIO CS + reg≠0 のチャネル選択不整合で ISR が来ないバグ /
タイムアウト時に SDK handle が busy 固着してバス全体が死ぬバグ)。upstream 報告候補。

## 開発環境

- ボード: XPI-iMX8MM の Cortex-M4 (`imx8mm_evk/mimx8mm6/m4` を流用)
- Zephyr: v4.3.0 (`apps/west.yml` で pin)
- ツールチェーン: apt の gcc-arm-none-eabi (gnuarmemb variant) —
  Zephyr SDK のダウンロード不要

## アプリ一覧 (`apps/`)

| アプリ | 用途 |
|--------|------|
| `can-gw` | **本命**。MCP2515 (ECSPI2) ⇄ rpmsg "kart-can" の双方向 CAN ゲートウェイ。Linux 側は kmm-yocto の `kart-rpmsg-can` が bind し `can0` を生やす |
| `can-sniff` | MCP2515 (ECSPI2) で CAN 受信 → UART4 (J64) 表示の疎通確認 |
| `rpmsg-echo` | rpmsg 疎通確認 (Zephyr の openamp_rsc_table サンプル構成) |
| `gpio-mu-test` | MU (IPM) + GPIO のみの最小構成。MU 受信 × ペリフェラル read の SoC リセット切り分け用 |
| `hello-world` | UART4 printk 疎通の最小雛形 |

ビルドは `apps/hello-world` の部分を各アプリ名に置き換える。can-gw の rpmsg /
attach 対応・BL31 先住起動などの詳細は kmm-yocto/docs/imx8mm-xpi-bringup/10-cortex-m4.md。

## 環境構築・ビルド (推奨: コンテナ)

必要なのは Docker だけ。`west-container.sh` が kas-container 相当の役割をする
(初回にイメージを自動ビルド。ツールチェーンは apt の gcc-arm-none-eabi /
gnuarmemb — Zephyr SDK 不要)。

```bash
./west-container.sh init -l apps     # 初回のみ
./west-container.sh update           # 初回のみ (zephyr + cmsis_6 + hal_nxp ~2.6GB)
./west-container.sh build -b imx8mm_evk/mimx8mm6/m4 apps/hello-world
# → build/zephyr/zephyr.elf
./west-container.sh --shell          # デバッグ用にコンテナ内シェル
```

リポジトリルートをホストと同一絶対パスでマウントするため、ビルドキャッシュは
ホスト直ビルドと相互運用できる。

## ホスト直で使う場合 (コンテナを使わない代替)

```bash
sudo apt install -y cmake ninja-build device-tree-compiler gcc-arm-none-eabi
python3 -m venv .venv && .venv/bin/pip install west pyelftools -r zephyr/scripts/requirements-base.txt
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=/usr
.venv/bin/west build -b imx8mm_evk/mimx8mm6/m4 apps/hello-world
```

## 実機へのデプロイ (XPI)

remoteproc (Linux から起動) の場合:

```bash
scp build/zephyr/zephyr.elf root@<XPI>:/tmp/
ssh root@<XPI> 'mount -o remount,rw /; cp /tmp/zephyr.elf /lib/firmware/; mount -o remount,ro /;
  echo stop > /sys/class/remoteproc/remoteproc0/state 2>/dev/null;
  echo -n zephyr.elf > /sys/class/remoteproc/remoteproc0/firmware;
  echo start > /sys/class/remoteproc/remoteproc0/state'
# コンソールは UART4 (J64) 115200
```

falcon SPL loadable + BL31 先住起動 (M4 が Linux より前に走り、Linux は attach) は
kmm-yocto の `kas/imx8mm-m4.yml` を参照。M4 バイナリ (`zephyr.bin`) を kmm-yocto の
`kart-falcon-itb` レシピに同梱する。

注意 (kmm-yocto/docs/imx8mm-xpi-bringup/04-pitfalls.md 参照):
- firmware_class の path パラメータは使わない (#24 の顛末) — /lib/firmware に置く
- クロックゲート等は Zephyr/HAL が面倒を見るが、CCGR のドメイン権限 (#25) を
  素通りするような自前レジスタ操作を足すときは要注意
