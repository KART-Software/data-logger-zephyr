/*
 * can_gw — CAN コントローラ ⇄ rpmsg "rpmsg-can" の双方向 CAN ゲートウェイ (main-app の機能)。
 * 仕様は can_gw.c の冒頭。
 */
#ifndef CAN_GW_H_
#define CAN_GW_H_

/* CAN コントローラの準備と、中継のスレッドを起こす (rpmsg が使えるようになってから始める) */
void can_gw_start(void);

#endif /* CAN_GW_H_ */
