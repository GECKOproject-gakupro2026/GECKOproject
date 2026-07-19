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

/* 録音開始(REC_START)を受けて連続ストリーミング転送状態に入る。Service::poll()が
 * recorder::Start() 直後に呼ぶ。緑LEDを点灯し、以後 SendRecInfo() が
 * recorder のリングから逐次ブロックを取り出して送り続ける。 */
void SendRecInfo_Arm();

/* 毎 poll 呼ぶ: ペーシングしつつ recorder::PopBlock() で1ブロックずつ取り出し
 * FRAME_CMD_REC_CHUNK として送る。録音中も録音停止後(残り送信中)も動く。
 * 全部送り切ったら FRAME_CMD_REC_END(総サンプル数・総ブロック数)を1回返して
 * ストリーミングを終え、緑LEDを消灯する。 */
void SendRecInfo();

/* 録音ストリーミング転送(録音中または残ブロック送信中)が進行中か。
 * Service::poll() がこれを見て SendStatus(センサーテレメトリ)を一時停止し、
 * 録音送信中は音声データ以外のBLE通信を行わないようにする。 */
bool IsRecTxActive();

} // namespace comm_ble

#endif /* COMM_BLE_HPP */
