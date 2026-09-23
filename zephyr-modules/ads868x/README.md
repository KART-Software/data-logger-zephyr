# zephyr-modules/ads868x — TI ADS8684/ADS8688 の Zephyr ADC ドライバ (out-of-tree)

標準 ADC API (`adc_channel_setup` / `adc_read` / `adc_raw_to_millivolts_dt`) で
ADS8684 (4ch) / ADS8688 (8ch) 16bit SAR ADC を読むドライバ。
zephyrproject-rtos/zephyr に PR として出す前提で、ドライバ本体・Kconfig・binding は
**Zephyr ツリーの対応パスと同じ配置・同じ中身**にしてある
(背景と計画: Issue #8)。

このリポでは Zephyr モジュールとして使う。アプリの `CMakeLists.txt` で
`find_package(Zephyr)` より前に

```cmake
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr-modules/ads868x)
```

を足すだけ (例: `apps/adc-dt`)。DT に `compatible = "ti,ads8688"` のノードがあれば
`CONFIG_ADC_ADS868X` は自動で y になる。

## 配置と upstream の対応

| このモジュール | upstream | 備考 |
|---|---|---|
| `drivers/adc/adc_ads868x.c` | 同じパス | ドライバ本体 |
| `drivers/adc/Kconfig.ads868x` | 同じパス | |
| `dts/bindings/adc/ti,ads868x-base.yaml` / `ti,ads8684.yaml` / `ti,ads8688.yaml` | 同じパス | レンジの写像は base の description に書いてある |
| `CMakeLists.txt` / `Kconfig` / `zephyr/module.yml` | (無し) | out-of-tree の接着剤 |
| `compat/zephyr_v4_3.h` | (無し) | v4.3.0 に無い名前 (`SPI_OP_MODE_CONTROLLER`) を `-include` で埋める。upstream には持ち込まない |

upstream 側で足すもの (PR に含める):

- `drivers/adc/CMakeLists.txt`: `zephyr_library_sources_ifdef(CONFIG_ADC_ADS868X adc_ads868x.c)`
- `drivers/adc/Kconfig`: `source "drivers/adc/Kconfig.ads868x"`
- `tests/drivers/build_all/adc/boards/native_sim.overlay`: `ti,ads8684` / `ti,ads8688` のテストノード
- `doc/releases/release-notes-<次版>.rst`: ADC 節に新ドライバ 1 行

## 設計

- **レンジ ↔ API**: `zephyr,gain` で span (2/5 → 2.5×Vref = 10.24V、4/5 → 1.25×Vref = 5.12V)、
  `zephyr,differential` で bipolar 版。bipolar のサンプルは符号付き 16bit で返す
  (デバイスの offset binary → 2 の補数) ので `adc_raw_to_millivolts_dt()` がそのまま正しい。
  ±0.625×Vref (2.56V) は gain 8/5 が API に無く非対応。
- **読み出し**: manual mode (MAN_Ch_n)。結果は次フレームに出るので、シーケンスの
  ch を順に選びながら前 ch の結果を拾い、最後に NO_OP で締める。AUTO_RST 巡回は未対応。
- **SPI**: mode 1、8bit ワード、1 フレーム 32 クロック。プログラムレジスタ書込は
  24 クロックで、3 バイト目に返る書込データ echo を検証する (datasheet 8.5.2.1)。
- **骨格**: `adc_context` + 取得スレッド (`adc_ads79xx.c` / `adc_mcp320x.c` と同型)。
  `channel_setup` はレンジレジスタを即書きし、その間 `adc_context_lock` で read と排他。

## 検証

`apps/adc-dt` (samples/drivers/adc/adc_dt 相当 + 全 ch 一括読み) を DEBIX (8MP) M7 で:

```bash
./west-container.sh build -b imx8mp_evk/mimx8ml8/m7 apps/adc-dt -d build-adcdt-8mp
# remoteproc で M7 に載せ、RAM console を読む (adc-test と同じ手順)
```

upstream の `scripts/checkpatch.pl -f drivers/adc/adc_ads868x.c` は 0 errors / 0 warnings。

2026-09-22 実機結果 (DEBIX M7、ADS8688 @ ECSPI1 5MHz、ch0 に 4.91V 源、他は開放):

| ch0 のレンジ | raw | mV |
|---|---|---|
| 0〜5.12V (gain 4/5) | 0xf5a4 | 4912 |
| ±5.12V (gain 4/5 + differential) | 0x7ace (符号付き +31438) | 4912 |
| ±10.24V (gain 2/5 + differential) | 0x3d6a (+15722) | 4913 |
| 0〜10.24V (gain 2/5) | 0x7ad8 | 4913 |

4 レンジとも一致 (adc-test の raw SPI 読み 4.91V とも一致)。開放 ch は unipolar で 2.56V (中点)、
bipolar では 1.97V を示すがこれは入力抵抗網のバイアス点の違いで、既知電圧の ch0 で確認済み。
全 ch 一括読み (1 シーケンス) の値は 1ch ずつの読みと一致 (パイプライン順序 OK)。
upstream main 版 (samples/drivers/adc/adc_dt + 同 overlay) も同値。
