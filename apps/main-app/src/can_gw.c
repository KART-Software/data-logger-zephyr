/*
 * can_gw — CAN コントローラ ⇄ rpmsg "rpmsg-can" の双方向 CAN ゲートウェイ (main-app の機能)。
 * apps/can-gw の CAN の部分を移したもの (ADC は別の機能として移す予定で、ここには入れない)。
 * CAN 側は chosen zephyr,canbus (8MM/M4: MCP2515 @ ECSPI2、8MP/M7: FlexCAN1)。
 *
 * Linux 側は kmm-yocto の rpmsg-can モジュールが NS 告知で bind して
 * CAN netdev rpmsgcan0 を登録する。以後 Linux アプリは SocketCAN 無修正。
 *
 *   CAN rx (ISRコールバック) → msgq → tx スレッド → rpmsg → rpmsgcan0
 *   rpmsgcan0 送信 → rpmsg → ept コールバック → can_send → CAN バス
 *
 * ワイヤ形式 (rpmsg-can.c と共通):
 *   - データフレーム 16B: { uint32 id; uint8 dlc; uint8 pad[3]; uint8 data[8] } (LE)
 *     id は Linux canid_t の慣例 (bit31=EFF, bit30=RTR)。
 *   - 制御メッセージ 8B: { uint8 magic=0xC7; uint8 cmd; uint8 flags; uint8 rsvd;
 *     uint32 arg }。長さで区別。Linux 側 candev の netlink 設定を反映する:
 *     bitrate/mode を受けて can_set_bitrate/can_set_mode、START/STOP で
 *     can_start/can_stop。M コア→Linux は状態変化 (bus-off 等) を EVT_STATE で返す。
 *   - それ以外の長さ (Linux probe の "hi" 等) は返信先アドレス学習にだけ使う。
 *
 * CAN の起動は Linux の `ip link up` (= START) 契機。M コア起動時は can_start せず、
 * bitrate/mode を停止中に受けてから start する (SocketCAN candev の流儀)。
 *
 * [8MM] 前提 (RDC): M4 が使うペリフェラル (ECSPI2/GPIO3/GPIO5) は、M4 起動前に
 * Linux (domain0) か BL31 から RDC PDAP を M4 専用 (0x0C) に設定しておくこと。
 * 共有 (0xFF) のままだと、MU/IPM を含むビルドではペリフェラル READ が
 * バスエラー → SoC ハードリセットになる (実測)。
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#include <openamp/open_amp.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(can_gw);

#include "can_gw.h"
#include "rpmsg_platform.h"

#if defined(CONFIG_SOC_MIMX8MM6)
/* ---- [8MM] MCP2515 (ECSPI2) まわりのクロックを M4 自身で enable ----
 * コンソールの UART4 のクロックは main.c が立てる。
 * Linux 側 DT は ecspi2 を disabled にした (M4 譲渡)。i.MX8MM の CCGR は
 * 4bit/domain のドメイン別要求フィールドで、domain0 (Linux) の devmem では
 * domain1 (M4) のフィールドに書けない (実測: 0x33 を書くと 0x03 になる)。
 * domain1 の要求が立っていないと M4 のレジスタアクセスで bus stall →
 * SoC ハードリセットになる (実測)。SDK の CLOCK_EnableClock と同じく
 * SET レジスタに全ドメイン分 (0x3333) を書く。M4 (domain1) からの write は
 * 自ドメインのフィールドに有効。ECSPI ドライバ init (banner 前) より先に
 * 走るよう PRE_KERNEL_1 priority 0 で行う。
 * TARGET_ROOT は Linux 側 (mcore_booted=1 + clk_ignore_unused) が enable を
 * 維持する取り決め (kmm-yocto imx8mm-xpi.dts のコメント参照)。 */
#define CCM_CCGR_ECSPI2_SET  0x30384084u	/* CCGR (0x4080) の SET */
#define CCM_CCGR_GPIO3_SET   0x303840D4u	/* CCGR (0x40D0) の SET */
#define CCM_CCGR_GPIO5_SET   0x303840F4u	/* CCGR (0x40F0) の SET */

static int m4_can_clocks_enable(void)
{
	/* clk-test (kmm-yocto m4/clk-test) での実測: M4 write は domain1
	 * フィールドのみに効き (0x3333 → 読み値 0x30)、これで ECSPI2 read が
	 * 2700 万回生還。can-gw が使う GPIO3 (INT)/GPIO5 (CS)、コンソールの
	 * UART4 も同様に立てる */
	*(volatile uint32_t *)CCM_CCGR_ECSPI2_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_GPIO3_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_GPIO5_SET = 0x3333;
	/* 前任 M4 が SPI 転送中に stop されていると ECSPI2 が動作状態のまま残り、
	 * SPI ドライバ init がハングする (state=running なのに無音、adc-test で実測)。
	 * M4 リセットはペリフェラルを初期化しないので毎起動 CONREG=0 で白紙に */
	*(volatile uint32_t *)0x30830008u = 0;
	return 0;
}
SYS_INIT(m4_can_clocks_enable, PRE_KERNEL_1, 0);

#elif defined(CONFIG_SOC_MIMX8ML8)
/* ---- FlexCAN1 クロックを M7 自身で enable (8MP) ----
 * Zephyr の ccm ドライバ (mcux_ccm_on) は CAN の CCGR ゲートを立てない
 * (case が無く素通り)。Linux 側 DT では flexcan1 を M7 譲渡で disabled に
 * する予定のため誰も立てず、FlexCAN ドライバ init のレジスタアクセスで
 * 止まる。8MM の CCGR 対応と同じく M7 自身で立てる。
 *
 * [falcon 対応 2026-09-10] CCGR ゲートだけでは不足。CAN1 クロック *root*
 * (CCM TARGET_ROOT @ 0x3038a200) の enable まで自前で立てる必要がある。
 * remoteproc 経路では U-Boot proper が root を立てていたが、falcon
 * (SPL→BL31→カーネル直行) では U-Boot proper を飛ばすため root が
 * 未 enable のまま (実機実測 0x00000000)。この状態で FlexCAN ドライバが
 * init すると get_rate は 24MHz を返すのに実クロックは 0Hz で、bit timing
 * が狂い CAN が一切通らない (実機: can0 RX=0)。root を enable + mux=0
 * (24MHz osc) + div=0 = 0x10000000 で立てる (ccm get_rate=24MHz と一致)。*/
#include <fsl_clock.h>

#define CCM_CAN1_CLK_ROOT	0x3038a200u	/* CCM TARGET_ROOT CAN1 (imx8mp) */
#define CCM_ROOT_EN_OSC24M	0x10000000u	/* bit28 enable, mux[26:24]=0 (osc_24m), div=0 */

static int m7_can_clocks_enable(void)
{
	*(volatile uint32_t *)CCM_CAN1_CLK_ROOT = CCM_ROOT_EN_OSC24M;
	CLOCK_EnableClock(kCLOCK_Can1);
	return 0;
}
SYS_INIT(m7_can_clocks_enable, PRE_KERNEL_1, 0);
#endif /* SoC */

/* ---- ワイヤ形式と Linux canid_t フラグ ---- */

struct rpmsg_can_wire {
	uint32_t id;
	uint8_t dlc;
	uint8_t pad[3];
	uint8_t data[8];
} __packed;

#define LINUX_CAN_EFF_FLAG 0x80000000U
#define LINUX_CAN_RTR_FLAG 0x40000000U
#define LINUX_CAN_EFF_MASK 0x1FFFFFFFU
#define LINUX_CAN_SFF_MASK 0x000007FFU

/* ---- 制御メッセージ (rpmsg-can.c と共通、8B。16B フレームと長さで区別) ----
 * Linux 側 candev の netlink 設定 (bitrate/mode) と up/down をここへ流す。
 * enum can_state は Zephyr と Linux で値が一致 (ACTIVE=0/WARN=1/PASSIVE=2/
 * BUS_OFF=3/STOPPED=4/SLEEPING=5) なので EVT_STATE は arg をそのまま渡す。 */
#define RPMSG_CAN_CTRL_MAGIC 0xC7
enum {
	RPMSG_CAN_CMD_SET_BITRATE = 1,	/* Linux→M4: arg = bitrate [bps] */
	RPMSG_CAN_CMD_SET_MODE    = 2,	/* Linux→M4: flags = mode ビット */
	RPMSG_CAN_CMD_START       = 3,	/* Linux→M4: CAN 起動 (ip link up) */
	RPMSG_CAN_CMD_STOP        = 4,	/* Linux→M4: CAN 停止 (ip link down) */
	RPMSG_CAN_EVT_STATE       = 5,	/* M4→Linux: arg = enum can_state */
};

struct rpmsg_can_ctrl {
	uint8_t magic;
	uint8_t cmd;
	uint8_t flags;
	uint8_t rsvd;
	uint32_t arg;
} __packed;

#define RPMSG_CAN_MODE_LISTENONLY (1u << 0)
#define RPMSG_CAN_MODE_LOOPBACK   (1u << 1)

/* ---- CAN ---- */

/* CAN コントローラは chosen で選ぶ (8MM: &can_mcp2515 / 8MP: &flexcan1) */
static const struct device *const can_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

K_MSGQ_DEFINE(can_rx_q, sizeof(struct can_frame), 64, 4);

/* Linux の START/STOP で駆動する (旧: main() で無条件 can_start)。
 * SET_BITRATE/SET_MODE は停止中でないと適用できないので、必要なら stop する */
static bool can_started;

/* 状態変化 → EVT_STATE の遅延送出。CAN の状態変化コールバックは ISR/
 * ドライバ文脈で走り得るので、そこでは値を積むだけにし、main ループが送る */
static atomic_t pending_state = ATOMIC_INIT(-1);
static K_SEM_DEFINE(state_sem, 0, 1);

#if defined(CONFIG_SOC_MIMX8MM6)
/* --- INT 可観測性: MCP2515 INT (GPIO3_IO24) のエッジをドライバとは独立の
 * callback で数え、IMR/PSR も直読みして stats に出す。gpio3 を Linux が
 * 掴んで IMR を消す時限バグ (kmm-yocto pitfalls #31) の切り分けで導入。
 * imr24 が 0 になったらこの類の再発 — stats 1 行で常時見張る */
static volatile uint32_t diag_edges;
static struct gpio_callback diag_int_cb;
static void diag_int_edge(const struct device *dev, struct gpio_callback *cb,
			  uint32_t pins)
{
	diag_edges++;
}
#define GPIO3_PSR (*(volatile uint32_t *)0x30220008u)
#define GPIO3_IMR (*(volatile uint32_t *)0x30220014u)
#endif /* CONFIG_SOC_MIMX8MM6 */

static void can_state_change_cb(const struct device *dev, enum can_state state,
				struct can_bus_err_cnt err_cnt, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(err_cnt);
	ARG_UNUSED(user_data);

	atomic_set(&pending_state, (atomic_val_t)state);
	k_sem_give(&state_sem);
}

/* 統計 (5 秒ごとにコンソールへ) */
static atomic_t stat_can_rx;		/* CAN → rpmsg 転送数 */
static atomic_t stat_can_rx_drop;	/* msgq 満杯ドロップ */
static atomic_t stat_rpmsg_drop;	/* rpmsg TX バッファ枯渇ドロップ */
static atomic_t stat_can_tx;		/* rpmsg → CAN 送信数 */
static atomic_t stat_can_tx_drop;	/* can_send 失敗 */

static void can_rx_cb(const struct device *dev, struct can_frame *frame,
		      void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (k_msgq_put(&can_rx_q, frame, K_NO_WAIT) != 0) {
		atomic_inc(&stat_can_rx_drop);
	}
}

/* ---- rpmsg ---- */

K_THREAD_STACK_DEFINE(thread_tx_stack, 1024);
K_THREAD_STACK_DEFINE(thread_ctl_stack, 1024);
static struct k_thread thread_tx_data;
static struct k_thread thread_ctl_data;

static struct rpmsg_endpoint can_ept;
/* Linux 側 ept アドレス。probe 時の "hi" (または任意の受信) で学習する */
static atomic_t peer_addr = ATOMIC_INIT(-1);

/* Linux (candev) からの制御メッセージを CAN コントローラへ反映する。
 * mng スレッド文脈から呼ばれる (CAN API 呼び出しは可)。設定は ip link up 時の
 * 数発だけなので、ここで can_stop/start しても vring 処理への影響は軽微 */
static void rpmsg_can_handle_ctrl(const struct rpmsg_can_ctrl *c)
{
	int ret;

	switch (c->cmd) {
	case RPMSG_CAN_CMD_SET_BITRATE:
		if (can_started) {
			can_stop(can_dev);
			can_started = false;
		}
		ret = can_set_bitrate(can_dev, c->arg);
		printk("can-gw: set_bitrate %u rc=%d\n", c->arg, ret);
		break;
	case RPMSG_CAN_CMD_SET_MODE: {
		can_mode_t mode = CAN_MODE_NORMAL;

		if (c->flags & RPMSG_CAN_MODE_LISTENONLY) {
			mode |= CAN_MODE_LISTENONLY;
		}
		if (c->flags & RPMSG_CAN_MODE_LOOPBACK) {
			mode |= CAN_MODE_LOOPBACK;
		}
		if (can_started) {
			can_stop(can_dev);
			can_started = false;
		}
		ret = can_set_mode(can_dev, mode);
		printk("can-gw: set_mode 0x%x rc=%d\n", (unsigned int)mode, ret);
		break;
	}
	case RPMSG_CAN_CMD_START:
		if (!can_started) {
			ret = can_start(can_dev);
			can_started = (ret == 0);
			printk("can-gw: start rc=%d\n", ret);
		}
		break;
	case RPMSG_CAN_CMD_STOP:
		if (can_started) {
			can_stop(can_dev);
			can_started = false;
			printk("can-gw: stop\n");
		}
		break;
	default:
		break;
	}
}

static int rpmsg_recv_can_callback(struct rpmsg_endpoint *ept, void *data,
				   size_t len, uint32_t src, void *priv)
{
	struct rpmsg_can_wire w;
	struct can_frame frame = {0};
	int ret;

	atomic_set(&peer_addr, (atomic_val_t)src);

	if (len == sizeof(struct rpmsg_can_ctrl)) {	/* 制御メッセージ (8B) */
		const struct rpmsg_can_ctrl *c = data;

		if (c->magic == RPMSG_CAN_CTRL_MAGIC) {
			rpmsg_can_handle_ctrl(c);
		}
		return RPMSG_SUCCESS;
	}
	if (len != sizeof(w)) {	/* "hi" ハンドシェイク等 */
		return RPMSG_SUCCESS;
	}
	memcpy(&w, data, sizeof(w));

	if (w.id & LINUX_CAN_EFF_FLAG) {
		frame.id = w.id & LINUX_CAN_EFF_MASK;
		frame.flags |= CAN_FRAME_IDE;
	} else {
		frame.id = w.id & LINUX_CAN_SFF_MASK;
	}
	if (w.id & LINUX_CAN_RTR_FLAG) {
		frame.flags |= CAN_FRAME_RTR;
	}
	frame.dlc = MIN(w.dlc, CAN_MAX_DLC);
	memcpy(frame.data, w.data, can_dlc_to_bytes(frame.dlc));

	/* mng スレッド文脈。MCP2515 の TX メールボックス待ちはしない —
	 * 満杯なら vring 処理を止めないためドロップ */
	ret = can_send(can_dev, &frame, K_NO_WAIT, NULL, NULL);
	if (ret == 0) {
		atomic_inc(&stat_can_tx);
	} else {
		atomic_inc(&stat_can_tx_drop);
	}
	return RPMSG_SUCCESS;
}

/* CAN → Linux。rpmsg が使えるようになってからエンドポイントを作る */
static void can_gw_tx_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct rpmsg_device *rdev = rpmsg_platform_wait();
	struct can_frame frame;
	struct rpmsg_can_wire w;
	int ret;

	if (!rdev) {
		LOG_ERR("can-gw: no rpmsg device");
		return;
	}

	/* NS 告知 → Linux 側で rpmsg-can が probe → rpmsgcan0 登録 */
	ret = rpmsg_create_ept(&can_ept, rdev, "rpmsg-can",
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       rpmsg_recv_can_callback, NULL);
	if (ret) {
		LOG_ERR("can-gw: could not create rpmsg-can endpoint: %d", ret);
		return;
	}
	printk("can-gw: rpmsg-can endpoint created, waiting for Linux peer\n");

	while (1) {
		k_msgq_get(&can_rx_q, &frame, K_FOREVER);

		uint32_t peer = (uint32_t)atomic_get(&peer_addr);

		if (peer == (uint32_t)-1) {
			continue;	/* Linux 側まだ — 捨てる */
		}

		memset(&w, 0, sizeof(w));
		w.id = frame.id;
		if (frame.flags & CAN_FRAME_IDE) {
			w.id = (frame.id & LINUX_CAN_EFF_MASK) |
			       LINUX_CAN_EFF_FLAG;
		} else {
			w.id = frame.id & LINUX_CAN_SFF_MASK;
		}
		if (frame.flags & CAN_FRAME_RTR) {
			w.id |= LINUX_CAN_RTR_FLAG;
		}
		w.dlc = MIN(frame.dlc, CAN_MAX_DLC);
		memcpy(w.data, frame.data, can_dlc_to_bytes(w.dlc));

		/* 非ブロック — TX バッファ枯渇 (Linux 停滞) はドロップ */
		ret = rpmsg_trysendto(&can_ept, &w, sizeof(w), peer);
		if (ret == 0 || ret == sizeof(w)) {
			atomic_inc(&stat_can_rx);
		} else {
			atomic_inc(&stat_rpmsg_drop);
		}
	}
}

/* 状態変化を Linux へ (EVT_STATE)、無ければ 5 秒ごとに統計をコンソールへ */
static void can_gw_ctl_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		enum can_state state;
		struct can_bus_err_cnt errs;

		/* 状態変化 (state_sem) で即起きて EVT_STATE を Linux へ。無ければ
		 * 5s ごとに J64 へ統計を出す */
		if (k_sem_take(&state_sem, K_SECONDS(5)) == 0) {
			atomic_val_t st = atomic_get(&pending_state);
			uint32_t peer = (uint32_t)atomic_get(&peer_addr);

			if (st >= 0 && peer != (uint32_t)-1) {
				struct rpmsg_can_ctrl e = {
					.magic = RPMSG_CAN_CTRL_MAGIC,
					.cmd = RPMSG_CAN_EVT_STATE,
					.arg = (uint32_t)st,
				};

				rpmsg_trysendto(&can_ept, &e, sizeof(e), peer);
				printk("can-gw: EVT_STATE %ld -> Linux\n", (long)st);
			}
			continue;	/* 統計は次の timeout 周で */
		}

		can_get_state(can_dev, &state, &errs);
		printk("--- can->rp %ld (qdrop %ld, rpdrop %ld) | rp->can %ld (drop %ld) | peer 0x%x | started=%d state=%d tx_err=%d rx_err=%d ---\n",
		       atomic_get(&stat_can_rx),
		       atomic_get(&stat_can_rx_drop),
		       atomic_get(&stat_rpmsg_drop),
		       atomic_get(&stat_can_tx),
		       atomic_get(&stat_can_tx_drop),
		       (uint32_t)atomic_get(&peer_addr),
		       can_started,
		       state, errs.tx_err_cnt, errs.rx_err_cnt);
#if defined(CONFIG_SOC_MIMX8MM6)
		printk("    int-diag: edges=%u lvl=%d imr24=%d\n",
		       diag_edges,
		       (int)((GPIO3_PSR >> 24) & 1),
		       (int)((GPIO3_IMR >> 24) & 1));
#endif
	}
}

void can_gw_start(void)
{
	const struct can_filter filter_all = {
		.flags = 0,
		.id = 0,
		.mask = 0,	/* mask 0 = 全 ID */
	};
	int ret;

	printk("can-gw: %s <-> rpmsg-can\n", can_dev->name);

	if (!device_is_ready(can_dev)) {
		printk("can-gw: CAN device NOT ready\n");
		return;
	}
	/* can_start は Linux (candev) の START = `ip link up` 契機で行う
	 * (SET_BITRATE/SET_MODE を停止中に受けてから起動するため)。ここでは
	 * 全 ID フィルタ登録と状態変化コールバック登録だけ */
	ret = can_add_rx_filter(can_dev, can_rx_cb, NULL, &filter_all);
	printk("can-gw: add_rx_filter rc=%d\n", ret);
	can_set_state_change_callback(can_dev, can_state_change_cb, NULL);

#if defined(CONFIG_SOC_MIMX8MM6)
	{
		const struct device *g3 = DEVICE_DT_GET(DT_NODELABEL(gpio3));

		gpio_init_callback(&diag_int_cb, diag_int_edge, BIT(24));
		gpio_add_callback(g3, &diag_int_cb);
	}
#endif

	k_thread_create(&thread_tx_data, thread_tx_stack,
			K_THREAD_STACK_SIZEOF(thread_tx_stack),
			can_gw_tx_task, NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_create(&thread_ctl_data, thread_ctl_stack,
			K_THREAD_STACK_SIZEOF(thread_ctl_stack),
			can_gw_ctl_task, NULL, NULL, NULL,
			K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
}
