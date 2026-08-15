/* hello_world — XPI-iMX8MM Cortex-M4 上の Zephyr 動作確認。
 * コンソールは UART4 (= XPI J64, 115200)。remoteproc でロードして使う:
 *   west build -b imx8mm_evk/mimx8mm6/m4 hello-world
 *   build/zephyr/zephyr.elf を /lib/firmware へ → echo start > .../state
 */
#include <zephyr/kernel.h>
#include <zephyr/version.h>

int main(void)
{
	int tick = 0;

	printk("kart M4: hello from Zephyr %s\n", KERNEL_VERSION_STRING);

	while (1) {
		k_sleep(K_SECONDS(1));
		printk("zephyr tick %d\n", ++tick);
	}
	return 0;
}
