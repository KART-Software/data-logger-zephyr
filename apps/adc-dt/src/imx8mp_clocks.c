/*
 * i.MX8MP (DEBIX) M7: ECSPI1 のクロックを自前で立てる (adc-test の m7_clocks_enable と同一)。
 *
 * Zephyr の ccm ドライバは ECSPI について「SYS_PLL1 / root 分周」でレートを計算するだけで
 * gate も root も立てない。falcon 経路では U-Boot proper も通らないので M7 が立てる。
 * ROOT120 = ECSPI1_CLK_ROOT: EN | MUX=4 (SYS_PLL1 800MHz) | PRE=/1 | POST=/16 → 50MHz。
 */
#include <zephyr/init.h>
#include <fsl_clock.h>

#define CCM_ECSPI1_CLK_ROOT	0x3038BC00u	/* CCM TARGET_ROOT120 */
#define ECSPI1_CONREG		0x30820008u

static int imx8mp_ecspi1_clocks_enable(void)
{
	*(volatile uint32_t *)CCM_ECSPI1_CLK_ROOT = (1u << 28) | (4u << 24) | (0u << 16) | 15u;
	CLOCK_EnableClock(kCLOCK_Ecspi1);
	CLOCK_EnableClock(kCLOCK_Gpio5);
	/* 前任が SPI 転送中に止められていても白紙から: CONREG=0 (ECSPI disable) */
	*(volatile uint32_t *)ECSPI1_CONREG = 0;
	return 0;
}
SYS_INIT(imx8mp_ecspi1_clocks_enable, PRE_KERNEL_1, 0);
