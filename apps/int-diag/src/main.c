/*
 * int-diag — MCP2515 の INT 線 (J62 pin22 = GPIO3_IO24) とレジスタの診断。
 *
 * 背景 (2026-08-24): CAN の物理送受信が「初回 TX 1 発のみ・RX 全滅・
 * エラーなし」で止まる。TX 完了/RX 通知はどちらも INT 経由なので INT 線
 * 不達が疑い。M4 は GPIO3 の RDC 上の正当な所有者なので、ここから直接
 * レベルとレジスタを突き合わせて「HAT / ボード / 配線 / ソフト」を切り分ける。
 *
 * 見るもの (1 秒毎に UART4 へ):
 *   - INT ピンの現在レベル (期待: 割り込みペンディング時 Low)
 *   - エッジ割り込みカウンタ (線にエッジが来ているか)
 *   - MCP2515 レジスタ (raw SPI READ 0x03): CANSTAT/CANINTF/EFLG/TEC/REC
 *     → CANINTF に要因が立っているのに INT=High なら HAT 出力〜pin22 の断線
 *
 * 使い方: ホストから mock CAN を流しながら scripts/try.sh int-diag で仮ロード。
 * リブートで can-gw に戻る。
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#define DBG ((volatile uint32_t *)0x20000100)

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
	/* ECSPI2 を白紙から (前任 M4 の SPI 途中 stop 対策、adc-test と同一) */
	*(volatile uint32_t *)0x30830008u = 0;
	return 0;
}
SYS_INIT(kart_m4_clocks_enable, PRE_KERNEL_1, 0);

/* MCP2515: mode0 (CPOL=0 CPHA=0)、8bit、CS は cs-gpios (GPIO5_IO13) */
static const struct spi_dt_spec mcp = SPI_DT_SPEC_GET(
	DT_NODELABEL(diag_mcp2515),
	SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);

static int mcp_read_reg(uint8_t addr, uint8_t *val)
{
	uint8_t tx[3] = { 0x03, addr, 0 };	/* READ */
	uint8_t rx[3] = { 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int ret = spi_transceive_dt(&mcp, &txs, &rxs);

	if (ret == 0 && val != NULL)
		*val = rx[2];
	return ret;
}

static volatile uint32_t edge_count;

static void int_edge_cb(const struct device *dev, struct gpio_callback *cb,
			uint32_t pins)
{
	edge_count++;
}

static struct gpio_callback int_cb_data;

int main(void)
{
	const struct device *gpio3 = DEVICE_DT_GET(DT_NODELABEL(gpio3));
	uint8_t canstat = 0, canintf = 0, eflg = 0, tec = 0, rec = 0;
	int ret, lvl, i;

	DBG[0] = 0x1D1A0001;
	printk("kart M4: int-diag (MCP2515 INT=GPIO3_IO24, regs via raw SPI)\n");

	/* ADS8688 (共有バスの相方) の CS を High に張ってデアサート */
	gpio_pin_configure(gpio3, 21, GPIO_OUTPUT_HIGH);

	/* INT ピン: 入力 + 両エッジ割り込みカウンタ */
	ret = gpio_pin_configure(gpio3, 24, GPIO_INPUT);
	printk("int-diag: gpio3.24 input cfg rc=%d\n", ret);
	ret = gpio_pin_interrupt_configure(gpio3, 24, GPIO_INT_EDGE_BOTH);
	printk("int-diag: edge irq cfg rc=%d\n", ret);
	gpio_init_callback(&int_cb_data, int_edge_cb, BIT(24));
	gpio_add_callback(gpio3, &int_cb_data);

	if (!spi_is_ready_dt(&mcp)) {
		printk("int-diag: SPI not ready\n");
		return 0;
	}

	/* 陽性対照: 溜まっている割り込み要因をクリアして INT を High に戻す。
	 * mock の次のフレーム受信で High->Low エッジが発生するはずなので、
	 * edges が増えれば「GPIO3_IO24 のエッジ割り込み配送」は正常と言える */
	{
		uint8_t tx[4] = { 0x02, 0x2C, 0x00, 0x00 };	/* WRITE CANINTF=0 */
		const struct spi_buf txb = { .buf = tx, .len = 3 };
		const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
		spi_write_dt(&mcp, &txs);
		/* EFLG の RX0OVR/RX1OVR (bit7:6) を BIT MODIFY でクリア */
		uint8_t tx2[4] = { 0x05, 0x2D, 0xC0, 0x00 };
		const struct spi_buf txb2 = { .buf = tx2, .len = 4 };
		const struct spi_buf_set txs2 = { .buffers = &txb2, .count = 1 };
		spi_write_dt(&mcp, &txs2);
		printk("int-diag: cleared CANINTF/EFLG (expect INT High then edges)\n");
	}

	for (i = 0; ; i++) {
		lvl = gpio_pin_get_raw(gpio3, 24);
		ret = mcp_read_reg(0x0E, &canstat);
		if (ret == 0) {
			mcp_read_reg(0x2C, &canintf);
			mcp_read_reg(0x2D, &eflg);
			mcp_read_reg(0x1C, &tec);
			mcp_read_reg(0x1D, &rec);
			printk("[%3d] INT=%d edges=%u | CANSTAT=%02x CANINTF=%02x "
			       "EFLG=%02x TEC=%u REC=%u\n",
			       i, lvl, edge_count, canstat, canintf, eflg, tec, rec);
		} else {
			printk("[%3d] INT=%d edges=%u | SPI err %d\n",
			       i, lvl, edge_count, ret);
		}
		DBG[1] = 0x1D1A0000 | (uint32_t)i;
		k_msleep(1000);
	}
	return 0;
}
