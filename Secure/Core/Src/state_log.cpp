/**
  ******************************************************************************
  * @file    state_log.cpp
  * @brief   state_log.hpp の実装: 状態遷移のSRAMリングログ。
  ******************************************************************************
  */
#include "state_log.hpp"

#include "main.h" /* HAL_GetTick */

namespace state_log
{
namespace
{
Record  ring_[kCapacity];
uint32_t head_ = 0;  /* 次に書き込む位置 */
uint32_t count_ = 0; /* 累積イベント数(kCapacityで飽和せず増え続ける) */

/* Step8の時刻同期でセットされるepochオフセット[ms]。
 * wall_ms = epochOffsetMs_ + HAL_GetTick()。未同期なら0で、wall=tickとなる。
 * time_sync.cpp / comm_service から SetEpochOffset() で更新する想定だが、
 * 本Stepではまだ呼ばれない(既定0)。 */
volatile uint32_t epochOffsetMs_ = 0;
} // namespace

void Push(Event ev, uint32_t retVal)
{
  uint32_t tick = HAL_GetTick();
  Record &r = ring_[head_];
  r.tick_ms = tick;
  r.wall_ms = epochOffsetMs_ + tick;
  r.event = static_cast<uint8_t>(ev);
  r.ret_val = retVal;
  head_ = (head_ + 1U) % kCapacity;
  count_++;
}

uint32_t Count() { return count_; }

uint32_t Size() { return (count_ < kCapacity) ? count_ : kCapacity; }

bool Get(uint32_t index, Record &out)
{
  uint32_t sz = Size();
  if (index >= sz)
  {
    return false;
  }
  /* 最古の保持レコードは、満杯なら head_(次書き込み=最古)、未満杯なら 0。 */
  uint32_t oldest = (count_ < kCapacity) ? 0U : head_;
  uint32_t pos = (oldest + index) % kCapacity;
  out = ring_[pos];
  return true;
}

/* Step8で使う: epochオフセットを設定する(内部リンク。宣言は time_sync 側)。 */
void SetEpochOffset(uint32_t offsetMs) { epochOffsetMs_ = offsetMs; }

} // namespace state_log
