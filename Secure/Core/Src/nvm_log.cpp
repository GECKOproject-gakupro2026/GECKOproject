/**
  ******************************************************************************
  * @file    nvm_log.cpp
  * @brief   nvm_log.hpp の実装。
  ******************************************************************************
  */
#include "nvm_log.hpp"

#include "state_log.hpp"

#include "b_u585i_iot02a_ospi.h"
#include "main.h"

namespace nvm_log
{
namespace
{

constexpr uint32_t kRegionBase = 0x600000U;      /* OTA staging/backupの外(11.3参照) */
constexpr uint32_t kSubsectorSize = 0x1000U;     /* 4 KB (BSP_OSPI_NOR_ERASE_4K)     */
constexpr uint32_t kSubsectorCount = 16U;        /* 64 KB ぶん = 16 x 4KB            */
constexpr uint32_t kRecordSize = 13U;            /* nvm_log::Record を packed で計算  */
constexpr uint32_t kRecordsPerSubsector = kSubsectorSize / kRecordSize; /* 315 */
constexpr uint32_t kCapacity = kSubsectorCount * kRecordsPerSubsector;  /* 5040 */

static_assert(sizeof(Record) == kRecordSize, "nvm_log::Record must stay packed/13B");

/* ヘッド(次に書く絶対レコード番号、0起点で増え続ける)は TAMP->BKP1R に持つ
 * (boot_guard.cppのBKP0Rと同じVBATドメイン作法。BKP0RはBootGuard使用中なので
 * 衝突しないBKP1Rを使う)。 */
void ensureBackupDomainReady()
{
  __HAL_RCC_RTCAPB_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
}

uint32_t headIndex()
{
  ensureBackupDomainReady();
  return TAMP->BKP1R;
}

void setHeadIndex(uint32_t v)
{
  ensureBackupDomainReady();
  TAMP->BKP1R = v;
}

/* OSPI初期化は遅延・多重呼び出し安全(BSP側がIsInitializedを見て2回目はno-op)。
 * ota.cppのensureNorReady()と同じ設定値を使う。 */
bool norReady_ = false;
bool ensureNorReady()
{
  if (norReady_)
  {
    return true;
  }
  BSP_OSPI_NOR_Init_t init = {};
  init.InterfaceMode = BSP_OSPI_NOR_OPI_MODE;
  init.TransferRate = BSP_OSPI_NOR_STR_TRANSFER;
  if (BSP_OSPI_NOR_Init(0, &init) != BSP_ERROR_NONE)
  {
    return false;
  }
  norReady_ = true;
  return true;
}

/* サブセクタ内の何レコード目まで有効に書けているか(消去直後は0)。
 * ヘッド絶対番号から逆算できるので専用の永続状態は要らない。 */
uint32_t subsectorOf(uint32_t absIndex) { return (absIndex / kRecordsPerSubsector) % kSubsectorCount; }
uint32_t offsetInSubsector(uint32_t absIndex) { return (absIndex % kRecordsPerSubsector) * kRecordSize; }

/* 消去待ち状態機械。Poll()から毎回呼ばれても即戻る - HAL_Delayは使わない。
 * Idle以外の間は新規書き込みを保留する(消去中のサブセクタには書けない)。 */
enum class EraseState : uint8_t { Idle, Erasing };
EraseState eraseState_ = EraseState::Idle;
uint32_t eraseSubsector_ = 0;

/* 最後に消去完了したサブセクタ番号(+1、0は「まだ何も消していない」の意味で
 * 予約)。offsetInSubsector(absIndex)==0のたびに無条件で再消去すると、消去
 * 完了直後の同一Poll()フロー内で「まだこのサブセクタ=Idleだから」の判定に
 * 再度合致し、書き込み前に何度も消去をやり直してしまう(実機で確認した
 * 無限消去ループの原因)。このサブセクタは既に消去済みだと分かれば
 * beginEraseIfNeeded()をスキップし、書き込みへ進む。 */
uint32_t lastErasedSubsectorPlus1_ = 0;

/* state_logの最古から何件をすでにNORへ反映したか(state_log::Count()基準の
 * 累積値。state_log自体は64件でSRAM上書きされるが、ここでの目的はPushされた
 * 順序をそのまま流し込むことなので、取りこぼし検知にだけ使う)。 */
uint32_t syncedStateLogCount_ = 0;

/* absIndexToWriteがサブセクタ先頭で、かつそのサブセクタがまだ消去済みで
 * ないなら消去を開始する。二重消去防止のためlastErasedSubsectorPlus1_を見る:
 * 消去完了直後にこの関数へ戻ってきても(同じPoll()呼び出し内、または次回)、
 * 対象サブセクタが「消去済み」と分かっていれば何もせず書き込みへ進ませる。 */
void beginEraseIfNeeded(uint32_t absIndexToWrite)
{
  if (offsetInSubsector(absIndexToWrite) != 0U)
  {
    return;
  }
  uint32_t subsector = subsectorOf(absIndexToWrite);
  if (lastErasedSubsectorPlus1_ == subsector + 1U)
  {
    return; /* このサブセクタは既に消去済み: 再消去しない */
  }
  eraseSubsector_ = subsector;
  uint32_t addr = kRegionBase + subsector * kSubsectorSize;
  if (BSP_OSPI_NOR_Erase_Block(0, addr, BSP_OSPI_NOR_ERASE_4K) == BSP_ERROR_NONE)
  {
    eraseState_ = EraseState::Erasing;
  }
  /* 発行に失敗したら次回Poll()で再試行する(eraseState_はIdleのまま)。 */
}

/* 消去完了を非ブロッキングで確認する。完了していれば true。
 * HAL_Delayは使わず、Poll()の呼ばれる頻度に完了待ちを分散させる。 */
bool pollEraseDone()
{
  return BSP_OSPI_NOR_GetStatus(0) != BSP_ERROR_BUSY;
}

} // namespace

void Poll()
{
  if (!ensureNorReady())
  {
    return;
  }

  if (eraseState_ == EraseState::Erasing)
  {
    if (!pollEraseDone())
    {
      return; /* まだ消去中: 今回は何もせず次回また確認する */
    }
    eraseState_ = EraseState::Idle;
    lastErasedSubsectorPlus1_ = eraseSubsector_ + 1U;
  }

  /* state_logに新規レコードがあれば1件だけ追記する(1 Poll() = 最大1件の
   * NOR書き込み。まとめて流し込まないのは、追記自体は数百usで軽いため必要
   * なく、かつ複数件を一度に処理すると消去待ちが重なった時にPoll()が長く
   * なりうるため)。 */
  uint32_t srcCount = state_log::Count();
  if (srcCount <= syncedStateLogCount_)
  {
    return; /* 追いついている */
  }

  /* state_logはSRAM上で64件を超えると上書きされる。まだ同期していない最古の
   * 絶対レコード番号(syncedStateLogCount_)が、現在SRAMに残っている最古の
   * 絶対番号(oldestHeldAbs)より古ければ、その間は取りこぼし済み-
   * (不揮発ログの目的は傾向把握であり、欠落があってもtick_ms/wall_msの連番で
   * 検出可能)、現在保持されている最古まで読み位置を進める。 */
  uint32_t oldestHeldAbs = srcCount - state_log::Size();
  if (syncedStateLogCount_ < oldestHeldAbs)
  {
    syncedStateLogCount_ = oldestHeldAbs;
  }

  state_log::Record rec;
  uint32_t srcIndexInRing = syncedStateLogCount_ - oldestHeldAbs;
  if (!state_log::Get(srcIndexInRing, rec))
  {
    return;
  }

  uint32_t absIndex = headIndex();
  if (eraseState_ == EraseState::Idle && offsetInSubsector(absIndex) == 0U)
  {
    beginEraseIfNeeded(absIndex);
    if (eraseState_ == EraseState::Erasing)
    {
      return; /* このレコードは次回Poll()で、消去完了後に書く */
    }
  }

  uint32_t addr = kRegionBase + subsectorOf(absIndex) * kSubsectorSize + offsetInSubsector(absIndex);
  Record out = {rec.tick_ms, rec.wall_ms, rec.event, rec.ret_val};
  if (BSP_OSPI_NOR_Write(0, reinterpret_cast<const uint8_t *>(&out), addr, sizeof(out)) ==
      BSP_ERROR_NONE)
  {
    syncedStateLogCount_++;
    setHeadIndex(absIndex + 1U);
  }
}

void Reset()
{
  if (!ensureNorReady())
  {
    return;
  }
  for (uint32_t s = 0; s < kSubsectorCount; ++s)
  {
    if (BSP_OSPI_NOR_Erase_Block(0, kRegionBase + s * kSubsectorSize, BSP_OSPI_NOR_ERASE_4K) !=
        BSP_ERROR_NONE)
    {
      continue; /* 個別失敗はスキップし、可能な範囲で消し続ける */
    }
    while (BSP_OSPI_NOR_GetStatus(0) == BSP_ERROR_BUSY)
    {
      HAL_Delay(5); /* 明示リセット操作(D2 LOG_RESET応答経路)なのでブロッキング許容 */
    }
  }
  eraseState_ = EraseState::Idle;
  lastErasedSubsectorPlus1_ = 0U + 1U; /* Resetで全サブセクタ消去済み: subsector 0 から書ける */
  setHeadIndex(0U);
  state_log::Reset();
  syncedStateLogCount_ = 0U;
}

uint32_t Count() { return headIndex(); }

uint32_t Size()
{
  uint32_t head = headIndex();
  return (head < kCapacity) ? head : kCapacity;
}

bool Get(uint32_t index, Record &out)
{
  if (!ensureNorReady())
  {
    return false;
  }
  uint32_t sz = Size();
  if (index >= sz)
  {
    return false;
  }
  uint32_t head = headIndex();
  /* 保持中の最古の絶対番号: 満杯なら head-kCapacity、未満杯なら0。 */
  uint32_t oldestAbs = (head > kCapacity) ? (head - kCapacity) : 0U;
  uint32_t absIndex = oldestAbs + index;
  uint32_t addr = kRegionBase + subsectorOf(absIndex) * kSubsectorSize + offsetInSubsector(absIndex);
  return BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&out), addr, sizeof(out)) == BSP_ERROR_NONE;
}

} // namespace nvm_log
