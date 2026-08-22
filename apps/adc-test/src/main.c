/*
 * adc-test — ADS8688 (8ch 16bit SAR ADC, ECSPI2 CS=GPIO3_IO21) の疎通確認。
 *
 * Zephyr に ADS8688 ドライバは無い (v4.3 で確認) ので raw SPI で叩く。
 * SPI 5MHz / mode1 (CPHA=1) は ESP32 版 data-logger の実績値
 * (kmm-yocto local/carrier-board-design.md)。
 *
 * 動作: AUTO_RST でチャネル自動巡回を開始し、10Hz で 8ch を読んで
 * UART4 (J64) に表示。TCMU 0x20000100 に進行ブレッドクラムも置く
 * (A53 から devmem 0x800100 で確認可 — kmm-yocto docs/10)。
 *
 * MCP2515 (HAT) が同じバスに刺さったままでも動くよう、MCP2515 の CS
 * (GPIO5_IO13) を High (デアサート) に張り付けてから開始する。
 *
 * ADS8688 コマンド (データシート 16bit コマンドレジスタ):
 *   NO_OP    0x0000  (継続。AUTO モード中は次チャネルの変換値が返る)
 *   AUTO_RST 0xA000  (ch0 から自動巡回を再スタート)
 * プログラムレジスタ書き込みは [addr:7][W=1][data:8] の 16bit + 応答 8bit:
 *   AUTO_SEQ_EN (0x01) = 0xFF  (ch0-7 全部を巡回対象に)
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#define DBG ((volatile uint32_t *)0x20000100)	/* TCMU ブレッドクラム */

/* --- CCGR domain1 self-serve (can-gw と同一。kmm-yocto pitfalls #25) --- */
#define CCM_CCGR_ECSPI2_SET  0x30384084u
#define CCM_CCGR_GPIO3_SET   0x303840D4u
#define CCM_CCGR_GPIO5_SET   0x303840F4u
#define CCM_CCGR_UART4_SET   0x303844C4u

static int kart_m4_clocks_enable(void)
{
	*(volatile uint32_t *)CCM_CCGR_ECSPI2_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_GPIO3_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_GPIO5_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_UART4_SET = 0x3333;
	*(volatile uint32_t *)0x3033050Cu = 0x2;	/* UART4_RXD input daisy */
	/* 前任 M4 が SPI 転送ループ中に stop されていると ECSPI2 が動作状態のまま
	 * 残り、Zephyr SPI ドライバの init がハングする (state=running なのに無音、
	 * 実測)。M4 リセットはペリフェラルを初期化しないので、毎起動ここで
	 * CONREG=0 (ECSPI disable) にして白紙から始める */
	*(volatile uint32_t *)0x30830008u = 0;
	return 0;
}
SYS_INIT(kart_m4_clocks_enable, PRE_KERNEL_1, 0);

/* mode1 = CPHA。8bit word・MSB first。CS は cs-gpios (GPIO3_IO21) 任せ */
static const struct spi_dt_spec adc = SPI_DT_SPEC_GET(
	DT_NODELABEL(adc_ads8688),
	SPI_OP_MODE_MASTER | SPI_MODE_CPHA | SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);

/* 16bit コマンド + 16bit 読み出しの 1 転送。AUTO モード中の応答 (今回の変換値)
 * は後半 16bit に乗る */
static int ads8688_cmd(uint16_t cmd, uint16_t *out)
{
	uint8_t tx[4] = { cmd >> 8, cmd & 0xFF, 0, 0 };
	uint8_t rx[4] = { 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int ret = spi_transceive_dt(&adc, &txs, &rxs);

	if (ret == 0 && out != NULL) {
		*out = ((uint16_t)rx[2] << 8) | rx[3];
	}
	return ret;
}

/* プログラムレジスタ書き込み: [addr<<1|1][data][dummy]。3 バイト目に echo が返る */
static int ads8688_prog_write(uint8_t addr, uint8_t val)
{
	uint8_t tx[3] = { (uint8_t)((addr << 1) | 1), val, 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };

	return spi_write_dt(&adc, &txs);
}

/* プログラムレジスタ読み出し: [addr<<1|0][00][readback] */
static int ads8688_prog_read(uint8_t addr, uint8_t *val)
{
	uint8_t tx[3] = { (uint8_t)(addr << 1), 0, 0 };
	uint8_t rx[3] = { 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int ret = spi_transceive_dt(&adc, &txs, &rxs);

	if (ret == 0 && val != NULL) {
		*val = rx[2];
	}
	return ret;
}

int main(void)
{
	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	uint16_t v[8];
	int ret;

	DBG[0] = 0xADC00001;
	printk("kart M4: adc-test (ADS8688 @ ECSPI2 CS=GPIO3_IO21, 5MHz mode1)\n");

	/* 共有バス上の MCP2515 (HAT) をデアサート (CS=GPIO5_IO13 を High) */
	if (device_is_ready(gpio5)) {
		gpio_pin_configure(gpio5, 13, GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_LOW);
	}

	if (!spi_is_ready_dt(&adc)) {
		printk("adc-test: SPI not ready\n");
		DBG[0] = 0xADC0DEAD;
		return 0;
	}
	DBG[0] = 0xADC00002;

	/* ESP32 版 data-logger (src/spi/ads8688.cpp) の実績手順に準拠:
	 * RST → RANGE → AUTO_RST。RST を打たないと、リロード時にチップが
	 * 前セッションの AUTO スキャン走行中のままレジスタ書き込みを受けて
	 * レンジが変換に反映されない (実測でハマった) */
	ret = ads8688_cmd(0x8500, NULL);	/* RST */
	printk("adc-test: RST rc=%d\n", ret);
	k_msleep(1);

	/* 8ch 全部を自動巡回対象にして AUTO_RST で開始 */
	ret = ads8688_prog_write(0x01, 0xFF);	/* AUTO_SEQ_EN = all */
	printk("adc-test: AUTO_SEQ_EN rc=%d\n", ret);
	/* 全 8ch を unipolar 0〜1.25×Vref = 0〜5.12V に (range reg 0x05+ch,
	 * code 0x06 = ESP32 版 data-logger の RANGE_4 と同一)。
	 * 換算: 生値/65535×5.12V (0x0000=0V, 0xFFFF=5.12V)。0-5V センサー前提 */
	for (int ch = 0; ch < 8; ch++) {
		ret = ads8688_prog_write(0x05 + ch, 0x06);
		if (ret != 0) {
			printk("adc-test: range ch%d rc=%d\n", ch, ret);
		}
	}
	/* 書き込み検証: 先頭と末尾を読み戻す (どちらも 0x06 が期待値) */
	{
		uint8_t seq = 0xAA, r0 = 0xAA, r7 = 0xAA;

		ads8688_prog_read(0x01, &seq);
		ads8688_prog_read(0x05, &r0);
		ads8688_prog_read(0x0C, &r7);
		printk("adc-test: readback AUTO_SEQ=%02x CH0_RANGE=%02x CH7_RANGE=%02x (want ff 06 06)\n",
		       seq, r0, r7);
	}
	ret = ads8688_cmd(0xA000, NULL);	/* AUTO_RST */
	printk("adc-test: AUTO_RST rc=%d\n", ret);

	DBG[0] = 0xADC00003;

	/* スループット計測モード: sleep 無しで連続スキャンし、毎秒レートを表示。
	 * CH3 (浮き = 2.56V 近傍のはず) が 0x7000..0x8fff を外れたら化けとカウント
	 * して SCLK を上げすぎたときの信号品質劣化を検知する */
	{
		int64_t t0 = k_uptime_get();
		uint32_t scans = 0, corrupt = 0;

		while (1) {
			/* ESP32 方式: NO_OP×7 + 8 個目に AUTO_RST (毎周 ch0 巻き戻し) */
			for (int ch = 0; ch < 8; ch++) {
				uint16_t cmd = (ch == 7) ? 0xA000 : 0x0000;

				if (ads8688_cmd(cmd, &v[ch]) != 0) {
					v[ch] = 0xFFFF;
				}
			}
			scans++;
			if (v[3] < 0x7000 || v[3] > 0x8fff) {
				corrupt++;
			}
			DBG[1] = scans;

			int64_t dt = k_uptime_get() - t0;

			if (dt >= 1000) {
				printk("rate: %u scans/s = %u kSPS, corrupt=%u, ch0=%04x ch3=%04x\n",
				       (uint32_t)(scans * 1000 / (uint32_t)dt),
				       (uint32_t)(scans * 8 / (uint32_t)dt),
				       corrupt, v[0], v[3]);
				t0 = k_uptime_get();
				scans = 0;
				corrupt = 0;
			}
		}
	}
	return 0;
}
