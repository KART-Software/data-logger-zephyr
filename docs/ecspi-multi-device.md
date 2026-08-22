# ECSPI に複数デバイスをぶら下げるときの罠と対策

MCP2515 (CAN) と ADS8688 (ADC) を ECSPI2 に同居させたとき (2026-08-22-23) に
踏んだ Zephyr `spi_mcux_ecspi` ドライバの問題と、その調査・対策の記録。
同じバスにデバイスを足す人は必ず読むこと。

## TL;DR

- **GPIO CS のデバイスは `reg = <0>` 以外だと素の Zephyr (v4.3.0) では動かない**
  → `patches/zephyr/0001` (upstream は bd47b8cc で修正済み、リリース未収録)
- **1 回の転送タイムアウトでバスの全デバイスが永久に死ぬ**
  → `patches/zephyr/0002` (upstream 未修正、報告ドラフトあり)
- パッチは `west-container.sh` が冪等適用する (README 参照)

## バグ 1: GPIO CS + reg != 0 のチャネル選択不整合

ECSPI は 1 本のバスに 4 つの「チャネル」(ネイティブ CS に対応する設定スロット)
を持ち、位相/極性/バースト長はチャネル別、転送に使うチャネルは
CONREG.CHANNEL_SELECT で選ぶ。

Zephyr ドライバは **configure では「GPIO CS なら Channel0」に設定を書く**のに、
**転送では DT の reg 値のチャネルを選んでいた**。GPIO CS + `reg != 0` の
デバイスは未設定チャネルで転送され、以下の質の悪い壊れ方をする:

- SCLK は出てデータも RXFIFO に入る (物理転送は成功)
- しかし RR (RXFIFO ready) フラグが立たず、割り込みが発火しない
- 完了 ISR が来ないので全転送が -ETIMEDOUT

**mcp2515 (`reg=0`) は偶然一致で動き、2 台目 (`reg=1`) だけ死ぬ**ため、
「共存問題」「SPI モード切替問題」に見える (実際どちらも実験で無罪証明済み)。
単独テストアプリを `reg=0` で書いてしまうと再現すらしない。

upstream は bd47b8cc (2026-06-22) で同一内容を修正済みだが、v4.4.0 までの
リリースには未収録。収録リリースに west.yml を上げたら patches/0001 は削除する。

## バグ 2: タイムアウト後に SDK handle が busy 固着

`spi_context_wait_for_completion()` がタイムアウトしても、ドライバは
後始末をしない。SDK の転送 handle が busy のまま残り、以後
**バスの全デバイス** (どの spi_dt_spec でも) の転送が
`kStatus_ECSPI_Busy` → -EIO で永久に失敗する。実測ではバグ 1 の
タイムアウトを引き金に、健全だった mcp2515 側も全滅した
(`E: Failed to read error register [-5]` が 5 秒毎に延々)。

修正はタイムアウト時に `ECSPI_MasterTransferAbort()` + CS 解除。SDK 自身が
正常完了時に同じ Abort を呼んで handle を idle に戻しており、API 設計どおりの
使い方。Zephyr では nrfx_spim が -ETIMEDOUT で同様の abort を行う先例がある。
upstream 未修正 (kmm-yocto local/zephyr-upstream-abort-fix にドラフト)。

## 有効だったデバッグ手法

1. **ドライバ ISR に実行カウンタ**を仕込む → 「isr 30->30」で
   *ISR ゼロ回* が一発確定 (「来たが処理されない」と区別できる)
2. **転送待機中に k_timer で横からレジスタを覗く** — 失敗後のダンプは
   後始末で状態が変わっている。「待っている最中」の
   INTREG/STATREG/TESTREG が証拠になる (RXFIFO にデータあり + RR enable +
   RR フラグ 0、という矛盾が撮れて確定した)
3. **STATREG のビット割はリファレンスで再確認** — RR は bit3。bit1 (TDR) を
   RR と思い込んで読み違え、一晩遠回りした
4. CONREG.CHANNEL_SELECT の「設定とチグハグな値」がコード非対称への入口だった

## 関連する周辺の罠 (このリポの実装で対策済み)

- **SPI 転送ループ中の M4 を remoteproc stop すると ECSPI が動作状態のまま残り、
  次の M4 の SPI ドライバ init がハングする** (state=running なのに無音)。
  M4 リセットはペリフェラルを初期化しない。→ 各アプリの SYS_INIT で
  CONREG=0 の白紙化を行う (can-gw / adc-test 実装済み)
- **rsc_table の無い ELF を remoteproc start するとカーネルが Oops** し、
  以後の start が mutex 待ちで永久ハング (要リブート)。Zephyr アプリは
  `CONFIG_OPENAMP_RSC_TABLE=y` を忘れない (rpmsg を使わなくても必要)
- **同期 can_send はバスに ACK 相手がいないと返ってこない**。周期送信は
  callback 付き非同期 (fire-and-forget) にする (can-gw の ADC 送信部参照)
