/**
  ******************************************************************************
  * @file    nvm_log.hpp
  * @brief   state_log(SRAMリング)を外部OSPI NORへ不揮発化する【Step D1】。
  *
  *          領域: 0x600000 以降(OTAのstaging 0x200000/backup 0x400000から
  *          最大距離、どちらの2MB領域にも重ならない - 信頼ルート不可侵)。
  *          kRegionSize ぶんを kSubsectorCount 個の4KBサブセクタとして使い、
  *          満杯になったサブセクタから順に消去してdrop-oldestで回す
  *          (SRAMリングのindex-remapセマンティクスを踏襲)。
  *
  *          【重要】消去(Erase_Block)はホットパス(poll()の毎回)では行わない。
  *          Poll() は「新規SRAMレコードを1件だけNORへ追記」または「消去待ちの
  *          非ブロッキングなポーリング1回」のどちらか軽い方だけをして即戻る。
  *          サブセクタが満杯になった時だけ消去が要るが、その消去コマンド発行も
  *          Poll() 呼び出し1回で完了し、完了待ち(最大400ms)はGetStatusを
  *          後続のPoll()呼び出しへ分散させる(HAL_Delayでブロックしない)。
  *
  *          ヘッドポインタ(次に書く絶対レコード番号)は各サブセクタ先頭8Bの
  *          ヘッダ(magic+seq)からNOR自体で復元できるようにしており、SRAMには
  *          復元後の値をキャッシュするだけ。VBATバックアップの無い本基板では
  *          USB完全電源断でTAMP->BKPxRの内容も失われることが実機で確認された
  *          (2026-07-17)ため、TAMP->BKP1Rには依存しない。
  ******************************************************************************
  */
#ifndef NVM_LOG_HPP
#define NVM_LOG_HPP

#include <cstdint>

namespace nvm_log
{

/* 呼び出し元(comm_service.cppのpoll())が毎周回呼ぶ。ブロッキングしない
 * (消去完了待ちはGetStatusの非ブロッキング確認のみ、HAL_Delay無し)。
 * 新規のstate_logレコードがあれば1件だけNORへ追記する。 */
void Poll();

/* 不揮発ログを全消去し、ヘッド/カウンタをリセットする(D2のLOG_RESETコマンド用)。
 * 呼び出し時点で消去待ち中でも安全に呼べる。ブロッキング(複数サブセクタ消去)-
 * ホストコマンド応答経路からの明示操作なのでpoll()の暗黙処理では呼ばない。 */
void Reset();

/* これまでにNORへ書いた総レコード数(state_log::Count()と同じ意味の永続版)。 */
uint32_t Count();

/* index番目(0=NOR上に残っている最古)のレコードを out にコピーする
 * (state_log::Record と同一レイアウト)。有効なら true。 */
struct __attribute__((packed)) Record
{
  uint32_t tick_ms;
  uint32_t wall_ms;
  uint8_t  event;
  uint32_t ret_val;
};
bool Get(uint32_t index, Record &out);

/* 保持件数(= min(Count(), 全サブセクタの収容可能件数))。 */
uint32_t Size();

} // namespace nvm_log

#endif /* NVM_LOG_HPP */
