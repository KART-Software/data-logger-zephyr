/* SCTR-read テスト: bare-metal can-sim は MU 割り込み有効 + SCTR (0x306C0008,
 * 非 MU ペリフェラル) の大量 READ で何時間も安定動作した (kmm GUI 駆動実績)。
 * 同じ SCTR read を Zephyr (CONFIG_IPM + mailbox0) でやって落ちるかを見る。
 *
 * - 落ちる → 「MU + 周辺 read」ではなく Zephyr 固有 init (SOC_ClockInit の
 *   AudioPLL/ルート mux 操作等) が真因 → bisect で特定・修正可能
 * - 生きる → ペリフェラルごとの差 (RDC PDAP など) が真因 → その線を精査
 */
#include <zephyr/kernel.h>

#define REG32(a) (*(volatile unsigned int *)(a))
#define SCTR_CNTCV_LO REG32(0x306C0008)
#define SCTR_CNTCV_HI REG32(0x306C000C)

int main(void)
{
	printk("kart M4: sctr-mu-test start (MU enabled, reading SCTR)\n");

	unsigned int n = 0;
	for (;;) {
		unsigned int hi, lo;
		do {
			hi = SCTR_CNTCV_HI;
			lo = SCTR_CNTCV_LO;   /* 非 MU ペリフェラル READ */
		} while (hi != SCTR_CNTCV_HI);
		(void)lo;
		n++;
		if ((n & 0xFFF) == 0) {
			printk("alive n=%u sctr=%u:%u\n", n, hi, lo);
		}
		if ((n & 0xF) == 0) {
			k_busy_wait(50);
		}
	}
	return 0;
}
