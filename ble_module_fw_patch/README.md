# BLE_AT_Server 修正済みソース（STM32WB5MMGモジュール側）

STM32CubeWB v1.18.0 の `Projects/P-NUCLEO-WB55.Nucleo/Applications/BLE/BLE_AT_Server`
をベースに、開発状況記録_2026-07-08.md の「BLE双方向通信テスト」節に記載した3つのバグを
修正したソース一式。ここにあるファイルで元プロジェクトの同名ファイルを上書きしてビルドする。

## 再現手順
1. STM32CubeWB v1.18.0 を取得（`git clone --branch v1.18.0` + サブモジュール
   `Drivers/STM32WBxx_HAL_Driver` / `Drivers/CMSIS/Device/ST/STM32WBxx` を init）
2. `Projects/P-NUCLEO-WB55.Nucleo/Applications/BLE/BLE_AT_Server/STM32CubeIDE/` を
   STM32CubeIDEでインポート
3. 本フォルダの `app_ble.c` / `p2p_stm.c` / `p2p_stm.h` / `ble_at_server.c` / `main.c`
   でそれぞれ以下のパスの同名ファイルを上書き:
   - `STM32_WPAN/App/app_ble.c`
   - `Middlewares/ST/STM32_WPAN/ble/svc/Src/p2p_stm.c`
   - `Middlewares/ST/STM32_WPAN/ble/svc/Inc/p2p_stm.h`
   - `Core/Src/ble_at_server.c`
   - `Core/Src/main.c`
4. ビルド（Debug）→ `BLE_AT_Server.elf` を生成
5. ボードのSW4=OFF/SW5=ONでオンボードST-LINKをSTM32WB5MMGに切替
6. `STM32_Programmer_CLI -c port=SWD mode=UR -d <elf> -v -rst` で書込
7. SW4=ON/SW5=OFFに戻す

## 修正内容
1. **60秒後のHardFaultバグ**: `Adv_Mgr()`が60秒タイマーで`CFG_TASK_ADV_UPDATE_ID`タスクを
   セットするが未登録でNULL相当の呼び出しになりCPU1がHardFault。`Adv_Update()`（何もしない）
   を追加し`UTIL_SEQ_RegTask`で登録
2. **切断後に広告が再開しない**: `HCI_DISCONNECTION_COMPLETE_EVT_CODE`ハンドラに
   `Adv_Request(APP_BLE_FAST_ADV)`を追加
3. **notifyペイロードが2バイト固定→20B→64Bに拡張**: P2Pサーバーの notify 特性はボタン通知専用に
   2バイト固定長で実装されていた。GATT特性長を2→20バイトに拡張し、任意ペイロードを
   送信する`P2PS_STM_Notify_Raw()`を追加。`Manage_Update_Charac()`から呼ぶよう変更。
   その後、U585側が39B(MiniStatus)を47Bフレームとして送るようになり20B上限を超過、
   ATコマンドは"OK"を返すもののGATT側で送信が黙殺される不具合が発生したため、
   2026-07-15にGATT特性長を20→**64バイト**へ再拡張（`aci_gatt_add_char`の第4引数）。
   これは録音チャンク送信(最大61B)にも収まる上限。
4. **AT通信バッファのオーバーフロー**: `ble_at_server.c`の`at_buffer`と`main.c`の
   extern宣言を64→160バイトに拡張（20Bのnotifyペイロードは0x表記で65文字を超えるため）

対応するU585側（本プロジェクト本体）の変更は
`Drivers/BSP/Components/stm32wb_at/stm32wb_at_client.c`（`client_buff_tx`を64→160B）と
`Secure/Core/Src/telemetry.cpp`（`bleAtBuffer`を64→160B）。
