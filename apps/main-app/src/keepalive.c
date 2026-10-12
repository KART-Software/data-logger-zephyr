/*
 * keepalive — M コアが動いていることを Linux に知らせる (kmm-yocto issue #19)。
 *
 * - rpmsg のチャネルを "mcore-keepalive" の名前で告知する。Linux 側は kmm-yocto の
 *   mcore-keepalive カーネルモジュールが bind し、M コアが動いているかを /sys に出す。
 *   CAN などの機能とは別のチャネルで、機能の状態は含めない (見るのは M コアが起動して
 *   動いているかだけ)。
 * - M コアは Linux 側の宛先を知らないので、Linux 側が最初に送ってくる 1 通 (モジュールが
 *   bind 時に送る "hi") の送り元を宛先として覚え、以後 1 秒ごとに送る。宛先は受け取るたびに
 *   更新する (モジュールを入れ直しても追従する)。
 * - メッセージ (リトルエンディアン、16 バイト):
 *     u32 magic = 0x414B434D ("MCKA")  u16 proto = 1  u16 len = 16
 *     u32 uptime_ms (M コアが起動してからの時間)  u32 seq (送るたびに 1 増える)
 * - 送れないとき (Linux 側がバッファを返していない) は待たずに次の周期へ回す。seq は
 *   送れたときだけ増やすので、Linux 側から見た抜けは seq の飛びで分かる。
 * - 試験用 (Kconfig の MAIN_APP_KA_TEST_PAUSE_AT_S): 送り始めてから決まった秒数で送るのを止め、
 *   決まった秒数後に再開する。Linux 側の途切れの検知を試すためで、製品では 0 (無効)。
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#include <openamp/open_amp.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(keepalive);

#include "keepalive.h"
#include "rpmsg_platform.h"

#define KA_MAGIC	0x414B434Du	/* "MCKA" (LE) */
#define KA_PROTO	1
#define KA_PERIOD_MS	1000

struct ka_msg {
	uint32_t magic;
	uint16_t proto;
	uint16_t len;
	uint32_t uptime_ms;
	uint32_t seq;
} __packed;

BUILD_ASSERT(sizeof(struct ka_msg) == 16, "keepalive message must be 16 bytes");

K_THREAD_STACK_DEFINE(thread_ka_stack, 1024);
static struct k_thread thread_ka_data;

static struct rpmsg_endpoint ka_ept;
static atomic_t ka_peer = ATOMIC_INIT(-1);

static int ka_recv_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		      uint32_t src, void *priv)
{
	ARG_UNUSED(ept);
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	ARG_UNUSED(priv);

	if ((uint32_t)atomic_set(&ka_peer, (atomic_val_t)src) != src) {
		printk("keepalive: peer 0x%x\n", src);
	}
	return RPMSG_SUCCESS;
}

static void ka_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct rpmsg_device *rdev = rpmsg_platform_wait();
	struct ka_msg msg = {
		.magic = KA_MAGIC,
		.proto = KA_PROTO,
		.len = sizeof(struct ka_msg),
	};
	uint32_t seq = 0;
	int64_t next;
#if CONFIG_MAIN_APP_KA_TEST_PAUSE_AT_S > 0
	int test_paused = 0;
#endif
	int ret;

	if (!rdev) {
		LOG_ERR("keepalive: no rpmsg device");
		return;
	}

	ret = rpmsg_create_ept(&ka_ept, rdev, "mcore-keepalive",
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       ka_recv_cb, NULL);
	if (ret) {
		LOG_ERR("keepalive: could not create endpoint: %d", ret);
		return;
	}
	printk("keepalive: mcore-keepalive endpoint created, waiting for Linux peer\n");

	/* 周期は起動からの絶対時刻で刻み、送る処理の時間でずれないようにする */
	next = k_uptime_get();
	while (1) {
		next += KA_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));

		uint32_t peer = (uint32_t)atomic_get(&ka_peer);

		if (peer == (uint32_t)-1) {
			continue;	/* Linux 側がまだ — 送り先が無い */
		}
#if CONFIG_MAIN_APP_KA_TEST_PAUSE_AT_S > 0
		/* 試験用: 送り始めた seq を秒の目安にして、決まった区間だけ送らない */
		if (seq >= CONFIG_MAIN_APP_KA_TEST_PAUSE_AT_S &&
		    test_paused < CONFIG_MAIN_APP_KA_TEST_PAUSE_LEN_S) {
			if (test_paused++ == 0) {
				printk("keepalive: TEST pause for %d s\n",
				       CONFIG_MAIN_APP_KA_TEST_PAUSE_LEN_S);
			}
			continue;
		}
#endif
		msg.uptime_ms = (uint32_t)k_uptime_get();
		msg.seq = seq;
		if (rpmsg_trysendto(&ka_ept, &msg, sizeof(msg), peer) >= 0) {
			seq++;
		}
	}
}

void keepalive_start(void)
{
	k_thread_create(&thread_ka_data, thread_ka_stack,
			K_THREAD_STACK_SIZEOF(thread_ka_stack),
			ka_task, NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
}
