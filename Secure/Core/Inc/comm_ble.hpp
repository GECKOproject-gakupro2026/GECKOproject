/**
  ******************************************************************************
  * @file    comm_ble.hpp
  * @brief   BLEモジュール(STM32WB5MMG, UART4のATコマンド)【port層】。
  *
  *          BLEモジュールを載せ替えるときは comm_ble.cpp だけを書き直す。
  *          呼び出し側(comm_service.cpp)はAT方言もUART番号も知らない。
  ******************************************************************************
  */
#ifndef COMM_BLE_HPP
#define COMM_BLE_HPP

#include "telemetry.hpp"   /* telemetry::FullStatus（送信するデータの型） */

namespace comm_ble
{

/* BLEモジュールを初期化し、P2Pサーバー＋アドバタイズを開始する。
 * 戻り値: モジュールとAT通信が成立すれば true */
bool Init();

/* モジュールとのAT通信が生きているか */
bool IsAlive();

/* セントラル(スマホ等)が接続中か */
bool IsConnected();

/* テレメトリを圧縮(MiniStatus 39バイト)してNotifyで送る。
 * 未接続なら何もしない。 */
void SendStatus(const telemetry::FullStatus &st);

/* BLE write(fe41)で受信したREC_START/STOPコマンドを取り出す。
 * 戻り値: 0=なし, 1=start, 2=stop。呼ぶと内部状態は0にクリアされる。 */
uint8_t TakeRecCmd();

/* 前回呼び出し以降にGATT write(fe41)を受信したか(=ホスト活動あり)を取り出す。
 * PC側の1Hz keep-aliveでボードをACTIVEに保つための、有線UART/TCPの
 * nsActivity相当。呼ぶと内部状態はfalseにクリアされる。 */
bool TakeHostActivity();

/* 録音停止(REC_STOP)を受けて「REC_INFO を返す準備」を立てる。Service::poll()が
 * recorder::Stop() 直後に呼ぶ。 */
void SendRecInfo_Arm();

/* 録音停止(REC_STOP)直後に、録音の総サンプル数と総チャンク数を FRAME_CMD_REC_INFO
 * でPCへ返す。PCはこれを見て REC_GET で1チャンクずつ取りに来る(ストップ&ウェイト)。
 * BLE notify は取りこぼしうるので、自動プッシュではなくPC主導のポーリングにして
 * 確実性を担保する。 */
void SendRecInfo();

/* PC からの録音チャンク取得要求(FRAME_CMD_REC_GET)を1件処理する。GATT write
 * コールバックが要求 seq をキューに積み、この関数を Service::poll() から毎回
 * 呼んでキューから1件取り出し、その seq の REC_CHUNK を1つ返す(割り込み文脈で
 * UART送信しないための分離)。キューが空なら何もしない。 */
void ServeRecGet();

/* 録音転送(REC_INFO 未応答 or REC_GET キューにデータあり)が進行中か。
 * Service::poll() がこれを見て SendStatus(センサーテレメトリ)を一時停止し、
 * 録音転送に notify リンクを譲る。 */
bool IsRecTxActive();

} // namespace comm_ble

#endif /* COMM_BLE_HPP */
