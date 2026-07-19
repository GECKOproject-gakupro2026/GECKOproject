# BLE_AT_Server 修正済みソース（STM32WB5MMGモジュール側）

STM32CubeWB v1.18.0 の `Projects/P-NUCLEO-WB55.Nucleo/Applications/BLE/BLE_AT_Server`
をベースに、開発状況記録_2026-07-08.md の「BLE双方向通信テスト」節に記載した3つのバグを
修正したソース一式。ここにあるファイルで元プロジェクトの同名ファイルを上書きしてビルドする。

## 再現手順
1. STM32CubeWB v1.18.0 を取得（`git clone --branch v1.18.0` + サブモジュール
   `Drivers/STM32WBxx_HAL_Driver` / `Drivers/CMSIS/Device/ST/STM32WBxx` を init）
2. `Projects/P-NUCLEO-WB55.Nucleo/Applications/BLE/BLE_AT_Server/STM32CubeIDE/` を
   STM32CubeIDEでインポート
3. 本フォルダの `app_ble.c` / `p2p_stm.c` / `p2p_stm.h` / `ble_at_server.c` / `main.c` /
   `p2p_server_app.c` でそれぞれ以下のパスの同名ファイルを上書き:
   - `STM32_WPAN/App/app_ble.c`
   - `STM32_WPAN/App/p2p_server_app.c`
   - `Middlewares/ST/STM32_WPAN/ble/svc/Src/p2p_stm.c`
   - `Middlewares/ST/STM32_WPAN/ble/svc/Inc/p2p_stm.h`
   - `Core/Src/ble_at_server.c`
   - `Core/Src/main.c`
   さらに `Drivers/BSP/Components/stm32wb_at/stm32wb_at.c`（U585側と共通のドライバファイル）
   も本プロジェクトのバージョンで上書きする(`str_received[160]`修正、下記5番)。
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
5. **(2026-07-15) notifyが常に30Bで切り詰められる根本原因**: `at_buffer`/`bleAtBuffer`は
   160Bに拡張済みだったが、共通ドライバ`Drivers/BSP/Components/stm32wb_at/stm32wb_at.c`の
   `stm32wb_at_Received()`内で受信行を`strcpy(&str_received[0], &buffer_rx[0])`により
   **固定64バイト**の`str_received`へコピーしており、64文字を超えるAT+BLE_NOTIF_VALコマンド
   （47バイトペイロードのhex表現は約94文字）がここでバッファオーバーフローしていた。
   `str_received[64]`→`str_received[160]`に拡張（`buffer_rx`と同じ160Bに揃える）。
   このファイルはU585/WB5MMG双方に同じソースがあるため、両方で修正が必要。
6. **(2026-07-15) BLE GATT write(fe41)がU585側に届かない・原因は2つ重なっていた**:
   a) `p2p_server_app.c`の`P2PS_STM_App_Notification()`の`P2PS_STM_WRITE_EVT`ハンドラは、
      ST公式サンプルのデモ用ロジック（`pPayload[0]`が`0x01`〜`0x06`のエンドデバイス選択
      バイトの場合のみLED制御として`ble_at_server_Send_evt(BLE_EVT_WRITE,...)`を呼ぶ）の
      ままだった。frame_codecフレーム（SOF=0xAA始まり）はこの範囲に一致せず、
      `stm32wb_at_BLE_EVT_WRITE_cb()`が一度も呼ばれていなかった。受信した生ペイロードを
      そのまま`BLE_EVT_WRITE`としてU585側へ転送する処理をデモロジックの手前に追加
      （`char_index=1`固定、`svc_index=1`固定、既存のLED選択デモはそのまま残す）。
   b) さらに write 特性(fe41)自体のGATT特性長が`p2p_stm.c`の`aci_gatt_add_char`で
      **2バイト固定**（ボタン2バイトのデモコマンド専用）のままだった。8バイトの
      REC_START/STOPフレームはこの上限を超え、Write Without Responseはエラーを返さない
      ため気づきにくいが、ATTレベルで黙って拒否されていた。notify特性と同じ64バイトに拡張。

対応するU585側（本プロジェクト本体）の変更は
`Drivers/BSP/Components/stm32wb_at/stm32wb_at_client.c`（`client_buff_tx`を64→160B）、
`Secure/Core/Src/comm_ble.cpp`（`bleAtBuffer`を64→160B）、
`Drivers/BSP/Components/stm32wb_at/stm32wb_at.c`（`str_received`を64→160B、5番と同じ修正）。

7. **(2026-07-19) notifyペイロードを64→248バイトに拡大（録音転送の高速化）**: BLE録音の
   1チャンクを大きくして転送チャンク数を約1/4に減らすため、notify特性長とAT関連バッファを
   まとめて拡張した。`CFG_BLE_MAX_ATT_MTU`は元々251なので無線層は対応済み、ボトルネックは
   GATT特性長とATコマンド文字列バッファだった。
   - WB5MMG側: `p2p_stm.c`のnotify特性 `aci_gatt_add_char` 第4引数を64→**248**
     （=ATT_MTU-3、1 notifyで送れる実質上限）。`ble_at_server.c`の`at_buffer`と`main.c`の
     extern宣言を160→**560**（240Bペイロードのhex行は約505文字）。
   - 共通ドライバ: `stm32wb_at.c`の`str_received`を160→**560**、`buffer_rx_size`/
     `buffer_rx_cursor`と`stm32wb_at_Init`引数を`uint8_t`→**`uint16_t`**（255文字上限を撤廃）、
     `stm32wb_at_ble.h`のNOTIF_VAL/INDIC_VALの`val_tab[64]`→**`val_tab[248]`**。
   - U585側: `stm32wb_at_client.c`の`client_buff_tx`を160→**560**、
     `Secure/Core/Src/comm_ble.cpp`の`bleAtBuffer`を160→**560**、`kRecChunkPayload`を58→**240**。
   これらのドライバファイル(`stm32wb_at.c/.h`, `stm32wb_at_ble.h`)はU585/WB5MMGで共通なので、
   U585側で編集したものをWBビルドツリーにコピーする(手順3と同じ運用)。
