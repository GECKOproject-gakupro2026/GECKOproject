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

/* NOR上の1レコードは3バイト: [delta_ms u16][packed u8]。long-term化のため
 * 旧13B(tick+wall+event+ret)から圧縮した。delta_ms は同一サブセクタ内で直前
 * レコードからの経過ms(サブセクタ先頭は0、基準はヘッダの base_wall_ms)。
 * packed は上位4bit=event(0〜12)、下位4bit=ret_val(0〜15、収まらなければ
 * kRetOverflow)。 */
struct __attribute__((packed)) StoredRecord
{
  uint16_t delta_ms;
  uint8_t  packed;
};
constexpr uint32_t kStoredRecordSize = sizeof(StoredRecord); /* 3 */
static_assert(kStoredRecordSize == 3U, "StoredRecord must stay packed/3B");

constexpr uint8_t  kRetOverflow = 0x0FU; /* ret_valが4bitに収まらなかった印 */
constexpr uint16_t kDeltaMax    = 0xFFFFU; /* delta_ms の上限(これ超は中継マーカー) */
constexpr uint8_t  kEventNone   = 0U;    /* state_log::Event::None(中継マーカーに流用) */

/* サブセクタ先頭ヘッダ。magic+seq(電源断からのヘッド復元用、7-1)に加え、
 * base_wall_ms(このサブセクタ先頭レコードの絶対時刻)を持つ。各レコードの
 * 絶対時刻は base_wall_ms + そのサブセクタ内 delta_ms の累積で復元する。
 * 消去直後は 0xFFFFFFFF なので magic 不一致で「未使用」と判別できる。 */
struct __attribute__((packed)) SubsectorHeader
{
  uint32_t magic;
  uint32_t seq;
  uint32_t base_wall_ms;
};
constexpr uint32_t kSubsectorHeaderSize = sizeof(SubsectorHeader); /* 12 */
constexpr uint32_t kSubsectorHeaderMagic = 0x4E564C32U; /* "NVL2"(旧13Bは NVL1) */
constexpr uint32_t kRecordsPerSubsector =
    (kSubsectorSize - kSubsectorHeaderSize) / kStoredRecordSize; /* 1361 */
constexpr uint32_t kCapacity = kSubsectorCount * kRecordsPerSubsector;  /* 21776 */

/* headIndex()のSRAMキャッシュ。起動後最初のアクセスで scanHeadIndexFromNor()
 * により復元し、以後はここを読み書きする(電源が入っている間はTAMP->BKP1Rの
 * 代わりに使うだけの通常変数)。 */
bool headCached_ = false;
uint32_t headCache_ = 0U;

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
uint32_t offsetInSubsector(uint32_t absIndex)
{
  return (absIndex % kRecordsPerSubsector) * kStoredRecordSize;
}

/* レコードのNOR上アドレス(サブセクタ先頭のヘッダぶんだけ後ろにずれる)。 */
uint32_t recordAddr(uint32_t absIndex)
{
  return kRegionBase + subsectorOf(absIndex) * kSubsectorSize + kSubsectorHeaderSize +
         offsetInSubsector(absIndex);
}

uint32_t subsectorHeaderAddr(uint32_t subsector)
{
  return kRegionBase + subsector * kSubsectorSize;
}

/* このサブセクタの最初のレコード(offsetInSubsector==0)を書く直前に、
 * 「絶対番号 seqAbsIndex から始まり、先頭レコードの絶対時刻は baseWallMs」
 * を示すヘッダを書き込む。 */
bool writeSubsectorHeader(uint32_t subsector, uint32_t seqAbsIndex, uint32_t baseWallMs)
{
  SubsectorHeader hdr = {kSubsectorHeaderMagic, seqAbsIndex, baseWallMs};
  return BSP_OSPI_NOR_Write(0, reinterpret_cast<const uint8_t *>(&hdr),
                             subsectorHeaderAddr(subsector), sizeof(hdr)) == BSP_ERROR_NONE;
}

/* 消去直後(0xFF埋め)かどうかを StoredRecord 全バイトで判定する。 */
bool isBlankStored(const StoredRecord &rec)
{
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&rec);
  for (uint32_t i = 0; i < sizeof(rec); ++i)
  {
    if (p[i] != 0xFFU)
    {
      return false;
    }
  }
  return true;
}

/* 起動後最初のアクセス時に一度だけ呼ばれる: 全サブセクタのヘッダをスキャンし、
 * 最大seqを持つ(=最新の書き込み対象になった)サブセクタを見つけたら、その中を
 * 先頭から走査して何件書かれているかを数え、headCache_ を復元する。
 * どのサブセクタにも有効なヘッダが無ければログは空(headCache_=0)。 */
void scanHeadIndexFromNor()
{
  uint32_t bestSubsector = 0;
  uint32_t bestSeq = 0;
  bool found = false;
  for (uint32_t s = 0; s < kSubsectorCount; ++s)
  {
    SubsectorHeader hdr;
    if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&hdr), subsectorHeaderAddr(s),
                           sizeof(hdr)) != BSP_ERROR_NONE)
    {
      continue;
    }
    if (hdr.magic != kSubsectorHeaderMagic)
    {
      continue; /* 未使用(消去済み)サブセクタ */
    }
    if (!found || hdr.seq >= bestSeq)
    {
      found = true;
      bestSeq = hdr.seq;
      bestSubsector = s;
    }
  }

  if (!found)
  {
    headCache_ = 0U;
    headCached_ = true;
    return;
  }

  /* bestSubsector内を先頭から走査し、空白(0xFF)レコードに当たるまで数える。 */
  uint32_t written = 0U;
  while (written < kRecordsPerSubsector)
  {
    StoredRecord rec;
    uint32_t addr =
        subsectorHeaderAddr(bestSubsector) + kSubsectorHeaderSize + written * kStoredRecordSize;
    if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&rec), addr, sizeof(rec)) != BSP_ERROR_NONE)
    {
      break;
    }
    if (isBlankStored(rec))
    {
      break;
    }
    written++;
  }

  headCache_ = bestSeq + written;
  headCached_ = true;
}

/* headIndex()/setHeadIndex()を呼ぶ前に必ず通す。1回目だけNORスキャンする。
 * 呼び出し元(Poll()/Get()/Count()/Size())はすべて先に ensureNorReady() を
 * 済ませているので、ここでOSPIアクセスして安全。 */
void ensureHeadCached()
{
  if (!headCached_)
  {
    scanHeadIndexFromNor();
  }
}

uint32_t headIndex()
{
  ensureHeadCached();
  return headCache_;
}

void setHeadIndex(uint32_t v)
{
  headCache_ = v;
  headCached_ = true;
}

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

/* 直前にNORへ書いたレコードの tick_ms。次レコードの delta_ms を
 * (今回の tick_ms - lastTickMs_)で求めるための基準(tick_ms は TIME_SYNC の
 * 影響を受けず連続なので差分の基準に適する)。まだ何も書いていない(or 起動直後)
 * なら lastWallValid_ が false で、その場合は次に書くレコードをサブセクタ先頭
 * 同様 delta=0 として扱い、base_wall_ms をそのレコードの wall_ms にする。 */
uint32_t lastTickMs_ = 0;
bool lastWallValid_ = false;

/* Get() の連続読み出し高速化キャッシュ。LOG_REQ は絶対番号を昇順に取得するので、
 * 直前に読んだ絶対番号(getCacheAbs_)の次を要求されたら、サブセクタ先頭から
 * 累積し直さずに前回の累積 wall(getCacheWall_)に今回の delta を足すだけで済む。
 * getCacheValid_ は Reset()/新規書き込みで無効化する。 */
bool getCacheValid_ = false;
uint32_t getCacheAbs_ = 0;
uint32_t getCacheWall_ = 0;

/* event(4bit)とret_val(4bit)を1バイトにパックする。ret_valが15を超える値は
 * kRetOverflow(0xF)に丸める(「本来の値は4bitに収まらなかった」印)。 */
uint8_t packEventRet(uint8_t event, uint32_t retVal)
{
  uint8_t ev4 = static_cast<uint8_t>(event & 0x0FU);
  uint8_t rv4 = (retVal <= 14U) ? static_cast<uint8_t>(retVal) : kRetOverflow;
  return static_cast<uint8_t>((ev4 << 4) | rv4);
}

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

  /* CommReturn(BLE送信の所要ms診断)はログの大半を占めるノイズで、状態遷移の
   * 記録には不要。かつ ret_val(所要ms)が4bitに収まらない唯一のイベントでも
   * あるので、NORには落とさず読み進めるだけにする。 */
  if (rec.event == static_cast<uint8_t>(state_log::Event::CommReturn))
  {
    syncedStateLogCount_++;
    return;
  }

  uint32_t absIndex = headIndex();
  bool atSubsectorStart = (offsetInSubsector(absIndex) == 0U);
  if (eraseState_ == EraseState::Idle && atSubsectorStart)
  {
    beginEraseIfNeeded(absIndex);
    if (eraseState_ == EraseState::Erasing)
    {
      return; /* このレコードは次回Poll()で、消去完了後に書く */
    }
  }

  /* delta_ms は tick_ms ベースで求める(wall_ms は TIME_SYNC で epoch*1000 へ
   * 不連続にジャンプするため差分の基準にできない。tick_ms は起動からの単調増加で
   * 連続)。絶対時刻の基準(base_wall_ms)だけはサブセクタヘッダに wall_ms で持ち、
   * 復元は base_wall_ms + (サブセクタ内 tick_ms 差分の累積) で行う。
   * サブセクタ先頭、または直前 tick が無い(起動直後)なら delta=0。 */
  uint32_t deltaFull;
  if (atSubsectorStart || !lastWallValid_)
  {
    deltaFull = 0U;
  }
  else
  {
    deltaFull = rec.tick_ms - lastTickMs_; /* tick_ms は単調増加(同一起動内) */
  }

  /* 65535ms を超える空白は、delta=65535 の中継マーカー(event=None)を書いて
   * 時間だけ進め、実レコードは次回Poll()で書く。イベント間隔は通常数秒で、
   * 中継が必要なのは長時間 IDLE のときだけ(実測 log では稀)。 */
  if (!atSubsectorStart && deltaFull > kDeltaMax)
  {
    StoredRecord marker = {kDeltaMax, packEventRet(kEventNone, kRetOverflow)};
    if (BSP_OSPI_NOR_Write(0, reinterpret_cast<const uint8_t *>(&marker), recordAddr(absIndex),
                           sizeof(marker)) == BSP_ERROR_NONE)
    {
      lastTickMs_ += kDeltaMax;
      setHeadIndex(absIndex + 1U);
    }
    return; /* 実レコードは次回Poll()で(syncedStateLogCount_は進めない) */
  }

  if (atSubsectorStart &&
      !writeSubsectorHeader(subsectorOf(absIndex), absIndex, rec.wall_ms))
  {
    return; /* ヘッダが書けなければこのレコードも書かない(次回再試行) */
  }

  StoredRecord out = {static_cast<uint16_t>(deltaFull),
                      packEventRet(rec.event, rec.ret_val)};
  if (BSP_OSPI_NOR_Write(0, reinterpret_cast<const uint8_t *>(&out), recordAddr(absIndex),
                         sizeof(out)) == BSP_ERROR_NONE)
  {
    lastTickMs_ = rec.tick_ms;
    lastWallValid_ = true;
    syncedStateLogCount_++;
    setHeadIndex(absIndex + 1U);
    getCacheValid_ = false; /* 書き込みでローテート/消去が起きうるので安全側で無効化 */
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
  lastWallValid_ = false;
  lastTickMs_ = 0U;
  getCacheValid_ = false;
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

  uint32_t firstAbsInSub = absIndex - (absIndex % kRecordsPerSubsector);

  StoredRecord sr = {};

  /* 高速パス: 直前に読んだ絶対番号の「次」を、同じサブセクタ内で要求された場合は
   * 前回の累積 wall に今回の delta を足すだけ(サブセクタ先頭からの再累積を省く)。 */
  if (getCacheValid_ && absIndex == getCacheAbs_ + 1U &&
      absIndex != firstAbsInSub /* サブセクタ先頭は素直に base から */)
  {
    if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&sr), recordAddr(absIndex), sizeof(sr)) !=
        BSP_ERROR_NONE)
    {
      return false;
    }
    uint32_t wall = getCacheWall_ + sr.delta_ms;
    out.wall_ms = wall;
    out.event = static_cast<uint8_t>((sr.packed >> 4) & 0x0FU);
    out.ret_val = static_cast<uint8_t>(sr.packed & 0x0FU);
    getCacheAbs_ = absIndex;
    getCacheWall_ = wall;
    return true;
  }

  /* 低速パス: 対象レコードのサブセクタ base_wall_ms を読み、そのサブセクタ先頭から
   * absIndex までの delta_ms を累積して絶対 wall_ms を復元する。 */
  uint32_t subsector = subsectorOf(absIndex);
  SubsectorHeader hdr;
  if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&hdr), subsectorHeaderAddr(subsector),
                        sizeof(hdr)) != BSP_ERROR_NONE)
  {
    return false;
  }

  uint32_t wall = hdr.base_wall_ms;
  for (uint32_t a = firstAbsInSub; a <= absIndex; ++a)
  {
    if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&sr), recordAddr(a), sizeof(sr)) !=
        BSP_ERROR_NONE)
    {
      return false;
    }
    wall += sr.delta_ms; /* 先頭レコードは delta=0 なので base のまま */
  }

  out.wall_ms = wall;
  out.event = static_cast<uint8_t>((sr.packed >> 4) & 0x0FU);
  out.ret_val = static_cast<uint8_t>(sr.packed & 0x0FU);
  getCacheValid_ = true;
  getCacheAbs_ = absIndex;
  getCacheWall_ = wall;
  return true;
}

} // namespace nvm_log
