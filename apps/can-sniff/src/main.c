/* can-sniff — MCP2515 経由で CAN フレームを受信して J64 に表示。
 * 全 ID 受信 (std/ext 両フィルタ) + 毎秒の統計行。
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

static const struct device *const can_dev =
	DEVICE_DT_GET(DT_NODELABEL(can_mcp2515));

static atomic_t rx_count;

static void rx_cb(const struct device *dev, struct can_frame *frame,
		  void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	atomic_inc(&rx_count);
	printk("rx id=%03x dlc=%d [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
	       frame->id, frame->dlc,
	       frame->data[0], frame->data[1], frame->data[2], frame->data[3],
	       frame->data[4], frame->data[5], frame->data[6], frame->data[7]);
}

int main(void)
{
	const struct can_filter filter_std = {
		.flags = 0,
		.id = 0,
		.mask = 0, /* mask 0 = 全 ID */
	};
	int ret;

	printk("kart M4: can-sniff (MCP2515 @ ECSPI2, 1Mbps)\n");

	if (!device_is_ready(can_dev)) {
		printk("CAN device NOT ready\n");
		return 0;
	}
#ifdef CONFIG_CAN_SNIFF_LOOPBACK
	/* バス相手 (ACK) 不要の内部折り返し — SPI/INT/ISR/rx_cb の全経路を
	 * 単体検証するモード。ワイヤには出ない */
	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	printk("set_mode(LOOPBACK) rc=%d\n", ret);
#endif
	ret = can_start(can_dev);
	printk("can_start rc=%d\n", ret);

	ret = can_add_rx_filter(can_dev, rx_cb, NULL, &filter_std);
	printk("add_rx_filter rc=%d\n", ret);

	while (1) {
		struct can_bus_err_cnt errs;
		enum can_state state;
		struct can_frame tx = {
			.id = 0x123,
			.dlc = 8,
			.data = {0x4B, 0x41, 0x52, 0x54, 0x00, 0x00, 0x00, 0x00},
		};
		static uint8_t seq;
		int txrc;

		k_sleep(K_SECONDS(2));
		tx.data[7] = seq++;
		txrc = can_send(can_dev, &tx, K_MSEC(500), NULL, NULL);
		printk("tx rc=%d seq=%d\n", txrc, seq);
		can_get_state(can_dev, &state, &errs);
		printk("--- rx: %ld | state=%d tx_err=%d rx_err=%d ---\n",
		       atomic_get(&rx_count), state, errs.tx_err_cnt,
		       errs.rx_err_cnt);
	}
	return 0;
}
