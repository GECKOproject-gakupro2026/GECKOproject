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
  *          ヘッドポインタ(次に書く絶対レコード番号)は各サブセクタ先頭ヘッダ
  *          (magic+seq+base_wall_ms)からNOR自体で復元できるようにしており、SRAM
  *          には復元後の値をキャッシュするだけ。VBATバックアップの無い本基板では
  *          USB完全電源断でTAMP->BKPxRの内容も失われることが実機で確認された
  *          (2026-07-17)ため、TAMP->BKP1Rには依存しない。
  *
  *          【long-term化(2026-07-19)】1レコードを旧13B(tick+wall+event+ret)
  *          から3B([delta_ms u16][packed u8])へ圧縮し、記録期間を約4.3倍
  *          (5024→21776件)に延ばした。delta_ms は同一サブセクタ内で直前レコード
  *          からの tick_ms 差分、packed は event(4bit)+ret_val(4bit)。絶対時刻は
  *          サブセクタヘッダの base_wall_ms に delta を累積して復元する。診断専用で
  *          値域の広い CommReturn(BLE送信所要ms)はNORには落とさない。
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

/* index番目(0=NOR上に残っている最古)のレコードを out にコピーする。
 * 有効なら true。
 *
 * 【NOR上の格納形式は3バイト】long-term化のため、NORには
 *   [delta_ms u16][packed u8]
 * だけを書く(旧13B → 3B、約4.3倍の記録期間)。delta_ms は「同じサブセクタ内で
 * 直前レコードからの経過ms」で、サブセクタ先頭レコードは 0(基準はサブセクタ
 * ヘッダの base_wall_ms)。packed は上位4bitが event(0〜12)、下位4bitが
 * ret_val(0〜15、収まらない値は 0xF=エラーマーカー)。65535ms を超える空白は
 * event=None/ret=0xF の中継マーカーレコードで表現する。
 *
 * 本構造体(Record)は Get() が「復元済みの絶対 wall_ms + event + ret_val」を
 * 返すための便宜的な形で、NOR上のレイアウトとは別物(PC側は従来の
 * LOG_RECORD_FMT のうち tick_ms を廃し wall_ms/event/ret_val を受け取る)。 */
struct __attribute__((packed)) Record
{
  uint32_t wall_ms;  /* 基準(サブセクタ base) + サブセクタ内 delta 累積 */
  uint8_t  event;
  uint8_t  ret_val;  /* 0〜15、0xF は「本来の値が4bitに収まらなかった」印 */
};
bool Get(uint32_t index, Record &out);

/* 保持件数(= min(Count(), 全サブセクタの収容可能件数))。 */
uint32_t Size();

} // namespace nvm_log

#endif /* NVM_LOG_HPP */
