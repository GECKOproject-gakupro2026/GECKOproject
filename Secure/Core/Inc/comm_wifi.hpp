/**
  ******************************************************************************
  * @file    comm_wifi.hpp
  * @brief   Wi-Fiモジュール(EMW3080)とTCPトランスポート【port層】。
  *
  *          Wi-Fiモジュールを別のものに載せ替えるときは comm_wifi.cpp だけを
  *          書き直す。この宣言と、呼び出し側(comm_service.cpp)は変えない。
  *
  *          【役割の境界】このファイルはバイト列の送受信までを担当する。
  *          受信バイトの解釈(OTAフレームの復号など)は呼び出し側の責任であり、
  *          ここには持ち込まない(OTA信頼ルートをSecure内の1箇所に集約するため)。
  ******************************************************************************
  */
#ifndef COMM_WIFI_HPP
#define COMM_WIFI_HPP

#include <cstddef>
#include <cstdint>

namespace comm_wifi
{

/* モジュールを初期化してAPに接続し、TCPサーバーを起動する。
 * 戻り値: Wi-Fiモジュールが生きていれば true（AP接続やTCP起動の失敗とは独立） */
bool Init();

/* APに接続してIPを取得済みか */
bool NetUp();

/* TCPクライアントが接続中か */
bool HasClient();

/* 組み立て済みのフレームをTCPクライアントへ送る。
 * 送信失敗が2回続いたらクライアントを切断する（切断検知はこの送信経路が権威）。 */
void SendFrame(const uint8_t *frame, size_t len);

/* 生バイト列をTCPクライアントへ送る（応答メッセージ用。失敗しても切断しない）。 */
void SendRaw(const uint8_t *data, int32_t len);

/* TCPの受信を1回分ポーリングする。
 *  - クライアント未接続時: 接続受付を試みる（1Hz）。戻り値は0
 *  - クライアント接続時:   受信バイトを buf に取り出す
 * 戻り値: buf に格納したバイト数（0以上）。呼び出し側がこのバイトを解釈する。 */
int32_t PollRecv(uint8_t *buf, size_t maxLen);

} // namespace comm_wifi

#endif /* COMM_WIFI_HPP */
