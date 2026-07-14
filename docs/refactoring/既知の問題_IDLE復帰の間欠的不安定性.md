# 既知の問題: IDLE復帰の間欠的不安定性（Step8で確認・リファクタリング起因ではない）

> **P1で解消（2026-07-14）**: IDLE突入時に呼んでいた `Comm_SetTelemetryEnabled(0U)` を削除し、
> `telemetryEnabled` を常時 `true` に保つよう `app_loop.c` を変更した（IDLE = ToFのみSLEEP、通信は継続）。
> Wi-Fi正常接続環境下で **10回連続リセット→IDLE経由→復帰が全PASS**することを確認済み。
> 詳細は `計画_IDLE再定義とトリガー抽象化と不要コード削除.md` のP1を参照。
> ただし別途、**Wi-Fi未接続（DHCP失敗）環境ではIDLE復帰が不安定になる新パターン**を検証中に発見した
> （下記「追記: Wi-Fi未接続時の別要因」参照）。これはP1の変更とは別の既存要因。

## 現象

`verify_regression.py` のパートB「IDLE復帰」（無通信でIDLE状態に入った後、keep-alive再開でACTIVEへ復帰しフレーム送信を再開できるか）が、**同一バイナリ・同一手順でもリセットのたびに成功したり失敗したりする**。

失敗時のパターン:
- IDLE遷移までは正常（無通信2秒後に0フレーム＝IDLEに入っている）
- keep-alive再開後、フレームが一切来なくなる（0フレーム）
- 以降、音声ストリーミング開始・OTA状態照会も無応答になる
- ICSR（`0xE000ED04`）は `0x00000000` で正常＝HardFaultではない。コアはHaltも可能で、クラッシュではなく通信スタックが応答不能に陥っている

IDLEを経由しない連続動作（keep-aliveを送り続けるだけ）は20秒以上安定して動作する。問題は「一度IDLE状態を経由した後の復帰」に限定される。

## 切り分け結果（Step8実施時、2026-07-14）

Step7完了時点で `verify_regression.py` を実行したところIDLE復帰がFAILしたため、Step7のコード変更（`secure_nsc.c` のベタ書きextern宣言を `comm_backend.h` へヘッダ化しただけで、ロジック変更は皆無）に起因するリグレッションかどうかを切り分けた。

手順:
1. `git worktree add` でStep6完了時点（commit `7ec80ba`）を別ディレクトリにチェックアウトし、独立してビルド
2. Step6版・Step7版のバイナリを何度も入れ替えて実機（B-U585I-IOT02A、COM9/SWD）に書き込み、`verify_regression.py` を反復実行
3. Secure=Step6版+NonSecure=Step7版、Secure=Step7版+NonSecure=Step6版、のように組み合わせを変えても検証

結果:
- Step6版・Step7版のどちらでも、リセットのたびにPASSしたりFAILしたりした（**同一バイナリで結果が変わる**）
- Step7のdiffは3ファイル（`comm_backend.h`新規、`comm_service.cpp`のinclude追加1行、`secure_nsc.c`のextern宣言7行をinclude 1行に置換）のみで、実行ロジックに変更なし
- 以上より、**この不安定性はStep7（本リファクタリング）に起因するものではなく、既存の間欠的な問題**と判断した

## 関連する過去の修正

過去のコミット `fb604f6`「バグ修正: ToF復帰・IDLE復帰不能 / ドキュメント整備」で同種の現象に対応済みだが、今回の観測から見て根本原因は完全には解消されていない可能性がある。

## 今後の対応

- リファクタリング（Step0〜Step8）のスコープでは対応しない（ロジック変更を伴う調査・修正はリファクタリングの「機械的移動のみ」というガードレールに反するため）
- 別タスクとして、IDLE→ACTIVE復帰時の通信スタック（UART RXのDMAアイドル検出、telemetryのポーリング周期、割り込み優先度まわり）を深掘り調査することを推奨
- 総合回帰テストでは、IDLE復帰以外の項目（ACTIVE維持・ToF/温度センサー・音声ストリーミング・OTA照会）が揃ってPASSする実行結果をもってリファクタリングの健全性確認とする

---

## 追記: P1実施時に発見したWi-Fi未接続時の別要因（2026-07-14）

### 現象

P1（IDLE再定義、`Comm_SetTelemetryEnabled(0U)` を呼ばない設計）を実装した直後、10回連続リセット検証を行ったところ**全10回FAIL**した。詳しく調べると、IDLE復帰後にテレメトリのレートが5Hz（IDLE用の低頻度）のまま50Hz（ACTIVE）に戻らず、やがて音声ストリーミング・OTA照会も無応答になっていた。

### 原因

このとき、PC側のモバイルホットスポット（SSID `U585-IOT02A`）が停止しており、ボードは起動時に以下のログを出していた:

```
[TLM] joining AP "U585-IOT02A"...
[TLM] MX_WIFI_Connect ret=0
[TLM] Wi-Fi connected, IP=0.0.0.0
[TLM] DHCP lease not obtained within 10 s
```

DHCPが失敗しIPが `0.0.0.0` のまま、`Secure/Core/Src/comm_service.cpp` の `Service::poll()` が2秒ごとに出す診断ログ `[PROF] per2s: ...` を見ると、`tcp=160〜188ms` という値が定常的に出ていた。これは `Service::pollTcp()` → `comm_wifi::PollRecv()` 内の `MX_WIFI_Socket_accept()`（`comm_wifi.cpp` L316）が、Wi-Fi未接続状態でも定期的に（5秒に1回、`nextAcceptTick`）呼ばれ、その都度ブロッキングしていたため。

P1でIDLE中も `telemetryEnabled` が常時 `true` になったことで、`Service::poll()` L574-588 の `pollTcp()` 呼び出しが**IDLE中も止まらなくなった**。以前（`Comm_SetTelemetryEnabled(0)` を呼んでいた頃）はIDLE中に `pollTcp()` 自体がスキップされていたので、この「Wi-Fi未接続時のacceptブロッキング」の影響はACTIVE中にしか出なかった。P1後はIDLE中もその影響を受けるようになった。

### 切り分け

PCのモバイルホットスポットを起動し、ボードが正常にDHCPでIPを取得できる状態にしてから同じ10回連続リセット検証を行ったところ、**全10回PASS**した。Wi-Fi未接続によるaccept遅延が原因であり、P1のコード変更自体に問題はないことを確認した。

### 結論・今後の対応

- **P1の変更（IDLE = 通信を止めない）は正しい**。Wi-Fi接続が正常な環境では間欠的不安定性は解消されている。
- ただし、**Wi-Fi未接続（DHCP失敗）状態が長時間続くと、`pollTcp()` のacceptブロッキングがACTIVE/IDLE問わず定期的にCPU時間を奪う**という別の潜在課題が可視化された。これはP1以前から存在した挙動（IDLE中のみ隠れていた）であり、新規のリグレッションではない。
- 対応候補（本計画のスコープ外・別タスク）:
  - Wi-Fi未接続時は `nextAcceptTick` の間隔をさらに伸ばす、またはDHCP失敗が続く場合はTCPサービス自体を諦めて再接続をバックオフする
  - `comm_wifi.cpp` L308 の「コンソールキー優先ガード」（`UART_FLAG_RXNE` を見ている）は、コンソールRXがDMA駆動のため機能していない既知の欠陥。これも合わせて見直すとよい
- 実運用（自作基板でWi-Fiモジュールを搭載しない、またはWi-Fiが常時接続される前提）では顕在化しない可能性が高いが、Wi-Fi接続の信頼性が低い環境では注意が必要。
