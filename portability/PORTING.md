# 自作基板への移植ガイド

このプロジェクトは、基板依存のコード(port層)を差し替えるだけで別の基板へ移せる構造になっている。

## レイヤの区分

| レイヤ | ファイル | 移植時の扱い |
|---|---|---|
| **契約層** | `comm_api.h` / `comm_backend.h` / `board_io.h` / `sensors.h` / `ns_audio.h` / `comm_dto.h` | **変更しない**。そのままコピー |
| **コア層** | `app_loop.c` / `ns_audio.c` / `comm_service.cpp` / `ota.cpp` / `boot_guard.cpp` / `frame_codec.c` | **変更しない**。そのままコピー |
| **port層** | `board_io.c` / `sensors.c` / `comm_wifi.cpp` / `comm_ble.cpp` / `comm_uart.cpp` / `audio_capture.cpp` / `mcu_info.cpp` / `console.cpp` / `stm32u5xx_hal_msp.c` | **書き直す**。新基板のピン配置・モジュールに合わせる |
| **アダプタ層** | `secure_nsc.c`(TZあり) / `comm_api_direct.c`(TZなし) | どちらか一方を選ぶ |

## ケース1: TrustZoneあり基板 (STM32U5/L5系)

現行構成をほぼそのまま持っていける。

1. 新プロジェクトをCubeMXで作る（TrustZone有効、Secure/NonSecureの2プロジェクト構成）
2. **契約層とコア層のファイルをそのままコピー**する
3. `secure_nsc.c` をコピーする（CMSEゲートウェイ。中身は変更不要）
4. **port層を新基板に合わせて書き直す**:
   - `board_io.c`: LED/ボタンのGPIOポート・ピン番号
   - `sensors.c`: 使うセンサーのBSPドライバ呼び出し。**`sensors.h` の4関数(Init/Refresh/Stop/Resume)のシグネチャは変えない**
   - `comm_wifi.cpp` / `comm_ble.cpp`: 使う通信モジュールのドライバ
   - `audio_capture.cpp`: 使うマイク/ADC
   - `mcu_info.cpp`: MCUが同系統(STM32U5)ならほぼそのまま
5. **フラッシュ配置とOTA設定を合わせる**:
   - リンカスクリプト(`*.ld`)のBank1(Secure)/Bank2(NonSecure)の配置
   - `ota.cpp` の `kNsFlashBase` / `kNsFlashSize`
   - `main.c`(Secure)の `MX_GTZC_S_Init` でペリフェラルのSecure/NonSecure属性
   - NonSecureの `.ns_appinfo` セクション配置（バージョン判定に使う）
6. `verify_regression.py` を新基板で走らせてPASSすることを確認する

**センサーが変わる場合**: `sensors.c` だけを書き直す。`FullStatus_t` のフィールド(温度・湿度・ToF距離など)に何を入れるかは自由だが、**構造体のレイアウト(165バイト)は変えない**。使わないフィールドは0のままでよい。PCツール側のパーサと必ず一致させること。

## ケース2: TrustZoneなし基板 (STM32F4/G4系など)

ハードウェアによるSecure/NonSecure隔離が無くなるが、**設計上の区分は維持する**。

### 何が変わるか

- `secure_nsc.c` は使えない（CMSEが無い） → 代わりに `comm_api_direct.c` を使う
- Secure/NonSecureの2プロジェクト構成にはならない → 1プロジェクトになる
- GTZC/SAUの設定は不要

### 何を維持するか（重要）

TrustZoneが無くても、**フラッシュを2つの領域に分ける設計はそのまま維持する**:

| 領域 | 中身 | 更新 |
|---|---|---|
| **基礎領域** (旧Secure) | 起動ローダー(Stage-0)、通信スタック、OTA機構、起動監視(BootGuard) | **OTAで更新しない**。壊れると復旧不能なので、書き込み器でしか更新しない |
| **アプリ領域** (旧NonSecure) | センサー、アプリロジック、LED制御 | **OTAで更新する**。壊れてもBootGuardが前のバージョンへ自動ロールバックする |

これを維持する理由: **OTAの安全性はハードウェア隔離ではなく「更新するものと、しないものを分ける」設計から来ている**。通信スタックやOTA機構が誤ったファームで壊れると、二度と更新できなくなる。TrustZoneはそれを強制する仕組みにすぎず、無くても設計原則は同じ。

### 手順

1. 新プロジェクトを作る（TrustZoneなし、1プロジェクト）
2. リンカスクリプトでフラッシュを2領域に分ける:
   ```
   FLASH_BASE  (rx) : ORIGIN = 0x08000000, LENGTH = 512K   /* 基礎領域 */
   FLASH_APP   (rx) : ORIGIN = 0x08080000, LENGTH = 512K   /* アプリ領域(OTA更新対象) */
   ```
   （サイズは基板のフラッシュ容量に合わせる）
3. 契約層・コア層をコピー
4. **`secure_nsc.c` の代わりに `comm_api_direct.c` をコピーしてビルドに含める**
5. port層を書き直す
6. `ota.cpp` のアドレス定数を新しいフラッシュ配置に合わせる
7. Stage-0（起動時のイメージ検証とジャンプ）を、`main.c`(Secure) の該当部分を参考に実装する:
   - アプリ領域の先頭ワード(スタックポインタ)とその次(Reset_Handler)が妥当か検証
   - `.ns_appinfo` 相当のマジック("NSAP")とバージョンを検証
   - 妥当ならアプリ領域へジャンプ（`VTOR` を書き換えてから）
   - 妥当でなければ基礎領域に留まり、OTAローダーとして動く
8. BootGuard（起動試行カウンタ）は、バックアップレジスタ(RTC/TAMP)があればそのまま使える

### アプリ層は無改造で動く

`app_loop.c` / `sensors.c` / `ns_audio.c` は `comm_api.h` しか知らないので、
アダプタが `comm_api_direct.c` に変わっても**1行も変える必要がない**。
これがこのリファクタリングの主目的だった。

## 移植後の検証

`docs/refactoring/verify_regression.py` を新基板で走らせる。COMポート番号だけ書き換えればよい。
6項目すべてPASSすれば、移植は成功している。
