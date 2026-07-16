/**
  ******************************************************************************
  * @file    state_log.hpp
  * @brief   状態遷移イベントのSRAMリングログ【診断用】。
  *
  *          リンク/状態機械の遷移を「時刻・イベントID・戻り値」の固定長
  *          レコードで内蔵SRAMのリングバッファに記録する。不揮発ではない
  *          (電源断/リセットで消える) - 汎用NVMが無く、NORはOTA専用のため。
  *          NVM化は将来の課題(docsのfollow-up参照)。
  *
  *          単一writer(スーパーループ)前提でロック無し。comm_service.cpp の
  *          nsCmdRing と同じ設計。
  ******************************************************************************
  */
#ifndef STATE_LOG_HPP
#define STATE_LOG_HPP

#include <cstddef>
#include <cstdint>

namespace state_log
{

/* 記録するイベント種別。値はログ読み出し側(PC/コンソール)と共有する意味付け。 */
enum class Event : uint8_t
{
  None          = 0,
  BleActive     = 1,  /* BLEリンク Idle->Active */
  BleIdle       = 2,  /* BLEリンク Active->Idle */
  TcpActive     = 3,
  TcpIdle       = 4,
  UartActive    = 5,
  UartIdle      = 6,
  LinkStandby   = 7,  /* LINK_STANDBYコマンド受信でリンクをIdle化 */
  CommReturn    = 8,  /* 通信(送信)処理が戻った: ret=所要ms */
  DeviceActive  = 9,  /* デバイス機械 IDLE->ACTIVE (Step6以降) */
  DeviceIdle    = 10, /* デバイス機械 ACTIVE->IDLE (Step6以降) */
  EnterComm     = 11, /* ENTER_COMMコマンド受信(厳密FSMの明示ウェイク要求) */
  StopComm      = 12, /* STOP_COMMコマンド受信(明示ACTIVE->IDLEヒント) */
};

/* 1レコード: 記録時のtick(ms)、イベント、任意の戻り値/付随値。
 * wall_ms は Step8の時刻同期で offset+tick を入れる(未同期なら tick と同じ)。 */
struct __attribute__((packed)) Record
{
  uint32_t tick_ms;
  uint32_t wall_ms;
  uint8_t  event;   /* Event */
  uint32_t ret_val; /* イベント固有の戻り値(所要ms、状態コード等) */
};

/* リング容量(エントリ数)。小さく保つ(SRAMのみ)。 */
constexpr uint32_t kCapacity = 64U;

/* 1イベントを記録する。満杯なら最古を上書き(リング)。ISRからは呼ばない。 */
void Push(Event ev, uint32_t retVal);

/* これまでに記録した総イベント数(オーバーフローで上書きされた分も含む累積)。 */
uint32_t Count();

/* index番目(0=最古の保持レコード)のレコードを out にコピーする。
 * 有効なら true。読み出しは保持中の最新 min(Count, kCapacity) 件。 */
bool Get(uint32_t index, Record &out);

/* 保持件数(= min(Count(), kCapacity))。 */
uint32_t Size();

/* wall_ms 計算用のepochオフセット[ms]を設定する(Step8の時刻同期で使用)。
 * wall_ms = offsetMs + HAL_GetTick()。未設定時は0でwall=tick。 */
void SetEpochOffset(uint32_t offsetMs);

} // namespace state_log

#endif /* STATE_LOG_HPP */
