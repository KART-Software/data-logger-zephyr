/*
 * main-app — M コア (8MP: Cortex-M7 / 8MM: Cortex-M4) の製品ファーム。
 *
 * 土台と機能をファイルで分ける:
 *   main.c            起動、コンソール (RAM + UART4 の tee)、各部の起動
 *   rpmsg_platform.c  Linux (imx_rproc) との rpmsg (attach 対応、MU は 1ms ポーリング)
 *   keepalive.c       M コアが動いていることを 1 秒ごとに Linux へ知らせる (チャネル "mcore-keepalive"、kmm-yocto issue #19)
 *   can_gw.c          機能: CAN コントローラ ⇄ rpmsg "rpmsg-can" の中継 (apps/can-gw から移した)
 * 機能 (ADC など) は、ファイルを分けて足していく。
 * ボード差分は boards/ の overlay と conf、SoC ガードに集める。
 *
 * 起動経路は 2 通りどちらでも動く: kmm-yocto の SPL/BL31 が先に起動して Linux が attach
 * する (製品)、Linux の remoteproc が ELF を読んで起動する (scripts/try.sh)。
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/libc-hooks.h>
#include <string.h>

#include "rpmsg_platform.h"
#include "keepalive.h"
#include "can_gw.h"

#if defined(CONFIG_SOC_MIMX8MM6)
/* [8MM] コンソールの UART4 のクロックを M4 自身で立てる。i.MX8MM の CCGR は 4bit/domain の
 * ドメイン別要求フィールドで、domain0 (Linux) からは domain1 (M4) の分を書けない。
 * SET レジスタに全ドメイン分 (0x3333) を書く (M4 の write は自ドメインの分にだけ効く)。
 * UART4 のパッドはリセット既定が ALT0=UART4 なので mux は不要、RX の入力 daisy だけ明示する。
 * 機能のクロック (CAN の ECSPI2 など) は、その機能のファイルが立てる (can_gw.c) */
#define CCM_CCGR_UART4_SET   0x303844C4u	/* CCGR76 (0x44C0) の SET — kmm-yocto pitfalls #25 実測 */

static int m4_clocks_enable(void)
{
	*(volatile uint32_t *)CCM_CCGR_UART4_SET = 0x3333;
	*(volatile uint32_t *)0x3033050Cu = 0x2;	/* UART4_RXD input daisy */
	return 0;
}
SYS_INIT(m4_clocks_enable, PRE_KERNEL_1, 0);
#endif /* CONFIG_SOC_MIMX8MM6 */

/* --- コンソール tee: printk を RAM console バッファと UART4 の両方へ。
 * RAM 側は CONFIG_RAM_CONSOLE により resource table の trace エントリとして公開され、
 * Linux から debugfs (remoteproc0/trace0) で読める。UART4 は Linux 不在時のための
 * 物理コンソール。RAM console 標準フック (PRE_KERNEL_1) を APPLICATION でこの tee に
 * 差し替える — それまでの分は RAM のみに入る */
extern char ram_console_buf[];
static int tee_pos;
static const struct device *tee_uart;

static int console_tee_out(int c)
{
	ram_console_buf[tee_pos] = (char)c;
	/* ram_console.c と同じ流儀: 末尾 1 バイトは常に NUL */
	tee_pos = (tee_pos + 1) % (CONFIG_RAM_CONSOLE_BUFFER_SIZE - 1);
	if (tee_uart != NULL) {
		uart_poll_out(tee_uart, (unsigned char)c);
	}
	return c;
}

static int console_tee_init(void)
{
	tee_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(tee_uart)) {
		tee_uart = NULL;
	}
	tee_pos = (int)strnlen(ram_console_buf, CONFIG_RAM_CONSOLE_BUFFER_SIZE - 1);
	__printk_hook_install(console_tee_out);
	__stdout_hook_install(console_tee_out);
	return 0;
}
SYS_INIT(console_tee_init, APPLICATION, 0);

int main(void)
{
	printk("main-app: start (%s)\n", CONFIG_BOARD_TARGET);

	rpmsg_platform_start();
	keepalive_start();
	can_gw_start();

	return 0;
}
