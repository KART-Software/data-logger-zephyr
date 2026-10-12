/*
 * keepalive — M コアが動いていることを Linux に知らせる (kmm-yocto issue #19)。
 * 仕様は keepalive.c の冒頭。
 */
#ifndef KEEPALIVE_H_
#define KEEPALIVE_H_

/* キープアライブのスレッドを起こす (rpmsg が使えるようになってから送り始める) */
void keepalive_start(void);

#endif /* KEEPALIVE_H_ */
