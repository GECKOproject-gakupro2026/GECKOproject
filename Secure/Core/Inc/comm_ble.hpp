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

/* recorder::Stop()後に録音済みADPCMデータをREC_CHUNK/REC_END生TLVで
 * notify送信するポンプ。呼ぶたびに数チャンクだけ送る(BLE 9600baud律速)。
 * 送信対象がなければ何もしない。Service::poll()から毎回呼ぶ想定。 */
void PumpRecTx();

/* BLE write(fe41)で受信したREC_START/STOPコマンドを取り出す。
 * 戻り値: 0=なし, 1=start, 2=stop。呼ぶと内部状態は0にクリアされる。 */
uint8_t TakeRecCmd();

/* 前回呼び出し以降にGATT write(fe41)を受信したか(=ホスト活動あり)を取り出す。
 * PC側の1Hz keep-aliveでボードをACTIVEに保つための、有線UART/TCPの
 * nsActivity相当。呼ぶと内部状態はfalseにクリアされる。 */
bool TakeHostActivity();

/* PumpRecTx()に「録音データの送信待ちがある」ことを伝える。
 * Service::poll()がrecorder::Stop()を呼んだ直後に呼ぶ。 */
void StartRecTx();

/* PumpRecTx()が録音データをまだ送信中(=bleRecSendPendingが立っている)か。
 * Service::poll()がこれを見てSendStatus(センサーテレメトリ)を一時停止し、
 * 音声送信の間はBLEの notify リンクを占有できるようにする。 */
bool IsRecTxActive();

/* FullStatus(165B)全体をFRAME_CMD_STATUS_FRAGの生TLVで3フラグメントに
 * 分割してnotify送信する。MiniStatus(SendStatus)は変更せず併用する。
 * 呼ぶたびに1フラグメントだけ送るので、Service::poll()から低頻度
 * (約1Hz)で呼ぶ想定。録音送信(IsRecTxActive())中は呼び出し元が止める。 */
void SendStatusFrag(const telemetry::FullStatus &st);

} // namespace comm_ble

#endif /* COMM_BLE_HPP */
