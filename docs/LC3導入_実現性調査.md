# LC3 / LE Audio 導入の実現性調査

> 調査日: 2026-07-17 / ブランチ: `refactor/layering`
> **本文書は調査結果のみ。実装はしない**(`docs/実装計画_統合.md` §7-4)。

## 結論

**現行構成(STM32WB5MMG + STM32CubeWB のコプロファーム)へのLC3/LE Audio導入は非推奨。**
コプロファームがCIS/BIG(Isochronous Channels、LE Audioの無線基盤)を提供しておらず、
自前実装は現実的な工数を大きく超える。一方で、現在のBLE経路のボトルネックは
コーデックの圧縮率ではなく制御UART(comm_ble.cpp 経由でSTM32WB5MMGへATコマンドを
送るUART4)だったため、[Phase A/Bのボーレート引き上げ](../ble_module_fw_patch/README.md)で
既に大部分が解決している(115200baud化・BLEテレメトリ10Hz化・録音チャンクポンプ増量、
いずれも本ブランチのコミット履歴で実施済み)。**LC3を追加で導入する動機自体が薄い。**

代替案として検討した「LE Audioスタックを使わず、U585側でLC3をソフトエンコードし、
既存のGATT notify(REC_CHUNK)経路でADPCMの代わりに送る」方式は技術的には成立しうるが、
音質・CPU負荷の優位性がADPCMに対してどれだけあるか未検証であり、今回の調査スコープでは
着手しない(下記「5. 代替案」参照)。

## 1. コプロファームのCIS/BIGサポート有無

調査対象: `C:\Users\takut\wb\STM32CubeWB\Projects\STM32WB_Copro_Wireless_Binaries\STM32WB5x\
Release_Notes.html`(本機で使用中のSTM32CubeWBインストール、本プロジェクトが
`ble_module_fw_patch/main.c` で書き込む `stm32wb5x_BLE_Stack_fw.bin` の兄弟バイナリ群)。

- 現在の書き込み対象は **`stm32wb5x_BLE_Stack_full_fw.bin`**(標準フル機能版)。
  サポートレイヤー: Link Layer, HCI, L2CAP, ATT, SM, GAP, GATT database。
  Isochronous Channels / CIS / BIG への言及は**一切なし**。
- 上位版 **`stm32wb5x_BLE_Stack_full_extended_fw.bin`** も確認したが、「certified BLE 5.3」を
  謳っているのは **GATT caching** と **Enhanced ATT** の2機能のみ。CIS/BIG/LE Audioの
  記載は同様に**一切なし**。Extended Advertisingのサポートはあるが、これはLE Audioの
  無線トランスポート(Isochronous Channels)とは別機能。
- リリースノート全文(`Release_Notes.html`)を"Isochronous" "LE Audio" "CIS" "BIG"で
  grepしても0件。STM32WB5x系コプロファームの現行ラインナップにこれらの機能を持つ
  バイナリは存在しない。
- **結論**: HCIレベルでのIsochronous Channelsサポートが無い以上、その上に載る
  LE Audioプロファイル(BAP/ASCS/PACS)は物理的に動かせない。ST公式のアップデートが
  出るのを待つ以外に、このコプロファームでのLE Audio対応経路は無い。

## 2. LC3コーデックライブラリの入手性・ライセンス

LC3自体(コーデックのビット処理)はLE Audioの無線トランスポート(CIS/BIG)とは独立した
コンポーネントであり、理論上はU585側で純粋なソフトウェアエンコーダ/デコーダとして
動かすこと自体は可能(§1の制約を回避できるわけではないが、§5の代替案の前提となる)。

- Bluetooth SIGはLC3のリファレンス実装を無償公開しており(Apache 2.0ライセンス系、
  `google/liblc3` 等のオープンソース実装が実質的な標準として広く使われている)、
  組み込み向けに移植された軽量版も存在する。ライセンス上の障害は無い。
- ST公式のLC3ライブラリ提供は現時点で確認できなかった(STM32CubeWBのMiddleware一覧に
  LC3関連パッケージは含まれていない)。使うなら外部ライブラリを自前で組み込む形になる。
- Cortex-M33(STM32U585、120MHz駆動)でのLC3エンコード負荷は、参考実装のベンチマークでは
  16kHz/1チャンネル程度なら実時間比で十分な余裕があるとされる例が多いが、本プロジェクトの
  実測は行っていない(要検証事項)。

## 3. 自前でLE Audio上位プロファイル(BAP/ASCS/PACS)を実装する規模

§1の通りコプロファームがCIS/BIGを提供しない以上、この選択肢はそもそも土台が無い。
仮にHCIレベルのIsochronous ChannelsをSTがサポートするコプロファームに切り替えたとしても:

- BAP(Basic Audio Profile)/ASCS(Audio Stream Control Service)/PACS(Published Audio
  Capabilities Service)は、GATTサービスとしての実装に加え、Isochronous Channelsの
  ストリーム確立手順(CIG/CIS設定、ASE状態遷移)を扱う必要があり、現行の
  `comm_ble.cpp`(ATコマンドで単一のP2P GATTサーバーを動かすだけの薄いラッパー)とは
  規模が一桁以上違う。市販のBLEスタック(Zephyr等)ではLE Audio対応に数千〜数万行規模の
  実装が割かれている。
- 本プロジェクトの通信構成(U585がSecure側でATコマンド越しにWB5MMGを操作する二段構成)
  では、プロファイル層の状態機械をどちらのMCUに置くかという設計判断も追加で必要になる。
- **見積もり**: 土台(コプロファームのCIS/BIG対応)が無い時点で規模見積もりの前提が
  成立しない。仮に土台が揃ったとしても、フル実装は本プロジェクトの他の全機能を
  合わせた規模に匹敵する程度の工数がかかると推定され、費用対効果が見合わない。

## 4. 現在のボトルネックの再確認

過去の調査(本ブランチの `perf(ble):` 系コミット)で判明した通り、録音転送やテレメトリ速度の
実際のボトルネックは**U585↔WB5MMG間の制御UART(comm_ble.cpp、元は9600baud)**であり、
BLEの無線速度そのものではなかった。この点は既に対処済み:

- `CFG_BLE_BAUDRATE` が115200へ引き上げ済み(`Secure/Core/Inc/app_config.h`)。
- BLEテレメトリ周期が10Hzまで引き上げ済み(`CFG_TLM_BLE_PERIOD_MS`)。
- 録音チャンクのポンプレートも引き上げ済み(`comm_ble.cpp` の `kChunksPerPump=8`)。
- 本セッションでさらに、BLE経由で全センサーデータを取得できる
  `FRAME_CMD_STATUS_FRAG` を追加済み(§7-3)。

つまり「LC3で圧縮率を上げてBLE帯域を節約する」動機となっていた前提(BLEが遅い)は
既に別の方法で解消されている。LC3導入によって追加で得られる価値は、ADPCMに対する
圧縮率・音質の差分のみに限定される。

## 5. 代替案: U585側LC3ソフトエンコード + 既存GATT notify経路

LE Audioスタック(CIS/BIG/BAP)を使わず、**現行のP2P GATT + `REC_CHUNK`/`REC_END`生TLV経路
はそのまま**に、送るペイロードをADPCMからLC3フレームに差し替える案。

- **技術的な成立性**: 成立する。LC3はトランスポート非依存のコーデックであり、
  「圧縮したバイト列をどう送るか」は既存のBLE notify経路で問題ない
  (現状のADPCMも同じ経路を使っている)。
- **音質/圧縮率の見込み**: LC3はADPCM(4bit/sample、4:1圧縮)よりビットレートあたりの
  音質で優位とされる(Bluetooth SIGの主張、一般的な評価でもOpus/SBCに近い効率とされる)。
  ただし本プロジェクトの16kHz/モノラルという用途でADPCMとの体感差がどれだけあるかは
  未検証。
- **CPU負荷**: 未検証。U585(Cortex-M33, 160MHz)側にエンコーダを追加することになり、
  既存の音声パイプライン(`audio_capture.cpp`、MDF1経由のPCM取得→ADPCM圧縮)に
  もう一段処理が増える。すでに50Hzテレメトリ・センサー取得・録音転送が同じメインループを
  共有しているため、追加負荷がループ周期(現状50Hz/20ms)を圧迫しないか実測が必要。
- **実装コスト**: 中程度。外部LC3ライブラリの移植・統合、`recorder.hpp`/`audio_capture.cpp`
  のエンコードパスの差し替え、PC側(`adpcm.py` 相当)のLC3デコーダ追加が必要。
  既存のフレーム構造(`REC_CHUNK`/`REC_END`)は流用できるため、通信プロトコル自体の
  変更は最小限で済む。
- **推奨**: 現時点では**着手しない**。§4の通りBLE速度のボトルネックは解消済みであり、
  LC3化で得られる音質向上が、追加の実装・検証コストに見合うかは別途ユーザー判断が必要。
  もし着手するなら、まずCPU負荷の実測(既存ループへの割り込み具合)から始めるのが妥当。

## 6. STM32WBA系ハードウェアへの移行要否

STM32WBA(Bluetooth 5.4、LE Audio/CIS/BIG対応を謳う新シリーズ)への移行が本格的な
LE Audio対応の唯一の現実的な経路だが、以下の理由で**本プロジェクトのスコープ外**と判断する。

- 本ボード(B-U585I-IOT02A)はSTM32WB5MMGを固定搭載しており、ハードウェア交換は
  基板の載せ替え(実質的な新規ボード設計)を意味する。
- 既存のTrustZoneアプリ層リファクタリング(Secure=通信/OTA、NonSecure=アプリ層)や
  センサー統合など、本プロジェクトの大半の資産はSTM32U585側にあり移行の影響は
  限定的だが、Secure側のBLE制御コード(`comm_ble.cpp`、ATコマンドプロトコル)は
  WBA系の通信方式次第で作り直しが必要になる可能性が高い。
- §4の通り現行構成での速度課題は解消済みであり、WBA移行を正当化するだけの
  具体的な要求(実際にLE Audioの音声品質・低遅延が必要な用途)が今のところ無い。

## まとめ表

| 選択肢 | 実現性 | 備考 |
|---|---|---|
| 現行WB5MMGでLE Audio(BAP/ASCS)本実装 | ❌ 不可 | コプロファームにCIS/BIG無し |
| 現行WB5MMGでLC3のみソフトエンコード+既存GATT経路 | ✅ 技術的に可能 | 動機(BLE速度)は既に解消済み、CPU負荷未検証 |
| STM32WBA系への移行 | ✅ 可能だが大規模 | 基板交換相当、本プロジェクトのスコープ外 |
| 何もしない(現状維持) | ✅ 推奨 | Phase A/Bで速度課題は解消済み |
