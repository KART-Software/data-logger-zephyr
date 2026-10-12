/*
 * rpmsg_platform — Linux (imx_rproc) との rpmsg を立ち上げる、main-app の土台。
 * 機能 (キープアライブ、CAN の中継、ADC) は rpmsg_platform_wait() で rpmsg_device を
 * 受け取り、自分のエンドポイントを作る。
 */
#ifndef RPMSG_PLATFORM_H_
#define RPMSG_PLATFORM_H_

#include <openamp/open_amp.h>

/* 管理スレッドを起こす (共有メモリ・リソーステーブル・vring の準備と、1ms ごとの受信の確認) */
void rpmsg_platform_start(void);

/* rpmsg が使えるようになるまで待つ。失敗したら NULL。何スレッドから呼んでもよい */
struct rpmsg_device *rpmsg_platform_wait(void);

#endif /* RPMSG_PLATFORM_H_ */
