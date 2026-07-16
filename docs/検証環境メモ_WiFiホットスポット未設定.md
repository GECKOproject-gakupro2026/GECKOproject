# 検証環境メモ: WiFi/TCPテストの一時パスについて

> 記録日: 2026-07-16

## 状況
現在の開発PCは従来の環境と異なり、**OTA/WiFi(TCP)経由の通信にはこのPCで
モバイルホットスポットを立てる必要がある**(`pc_side/wifi_hotspot.ps1` 参照)。
現状そのホットスポットは未設定で、ボードは AP に join できない。

## 影響
- `docs/refactoring/verify_regression.py` のうち、WiFi/TCP に依存する項目、および
  ホットスポット未設定に起因するタイミング揺らぎ(keep-alive 送信中の ACTIVE 維持
  フレーム数が基準に満たない等)が **FAIL することがある**。
- これは firmware の状態遷移リファクタ(通信リンク状態機械)の欠陥ではなく、
  **測定側の環境要因**。実際、UART(COM9)経由ではテレメトリ・コマンド応答
  (例: LINK_STANDBY の ACK)は正常に流れている。

## 方針(ユーザー指示)
- WiFi/TCP に依存する検証は **一旦パス**する。
- 各 Step の検証は「その Step が触った機能」を UART/COM9 経由や BLE で直接確認して
  コミットし、状態遷移リファクタの残りの Step を先に全て実装完了させる。
- WiFi 依存の完全な回帰確認は、**モバイルホットスポットを立てられる環境で後日
  まとめて実施**する。

## 復帰手順(後日)
1. `pc_side/wifi_hotspot.ps1 -Action start`(または Windows 設定でモバイルホットスポットを
   2.4GHz・SSID/パスワードは `Secure/Core/Inc/app_config.h` の `CFG_WIFI_SSID`/
   `CFG_WIFI_PASSWORD` に一致させて起動)。
2. ボードをリセットし AP join → IP 取得(UART ログ `[TLM] Wi-Fi connected, IP=...`)を確認。
3. `python docs/refactoring/verify_regression.py` を全項目 PASS まで確認。
4. TCP クライアント接続時のリンク排他(BLE と TCP 同時、UART 割り込み)も実機確認。
