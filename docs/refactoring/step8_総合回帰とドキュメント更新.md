# Step 8: 総合回帰テストとドキュメント更新（最終ステップ）

## 目的
リファクタリング全体が既存機能を壊していないことを、**各ステップの簡易検証ではカバーしきれない重い項目**まで含めて確認し、ドキュメントを新しい構造に合わせて更新する。

## 前提条件
- Step 7 までがすべて完了しコミット済み
- ブランチ `refactor/layering` にいる

---

## パート A: 総合回帰テスト

各ステップでは `verify_regression.py`（軽量な6項目）を回してきた。ここでは**OTAの実ファーム往復**と**ロールバック機構**という、時間はかかるが最も重要な項目を確認する。

### A-1. 事前準備: ビルドとバージョン確認

```bash
CUBEIDEC="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeidec.exe"
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
OBJCOPY="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-objcopy.exe"
HWS="C:/temp/hws_refactor"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

# クリーンビルド（キャッシュの影響を排除する）
rm -rf "$REPO/Secure/Debug" "$REPO/NonSecure/Debug"

"$CUBEIDEC" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "$HWS" -importAll "$REPO" \
  -build "B-U585I-IOT02A_Secure/Debug" -build "B-U585I-IOT02A_NonSecure/Debug"

# 両方を書き込んでリセット
"$CLI" -c port=SWD mode=UR -d "$REPO/Secure/Debug/B-U585I-IOT02A_Secure.elf" -v
"$CLI" -c port=SWD mode=UR -d "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.elf" -v
"$CLI" -c port=SWD mode=UR -rst
sleep 14
```

**期待結果**: 両方 `0 errors`、`Download verified successfully`

### A-2. 基本回帰（6項目）

```bash
cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: **PASS**

### A-3. OTA実ファーム往復（UART経由）

**これが最重要項目**。OTA機構が壊れていたら、以降ファームを更新できなくなる。

```bash
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
OBJCOPY="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-objcopy.exe"

# NonSecureのbinを作る
"$OBJCOPY" -O binary "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.elf" \
                     "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.bin"

cd "$REPO/pc_side/status_monitor"

# ボードを起こしてからOTA実行
python -c "
import serial, time
with serial.Serial('COM9', 921600, timeout=0.2) as s:
    for _ in range(4):
        s.write(b'\x00'); time.sleep(0.3); s.read(8192)
print('ボードを起こしました')
"

python ota_update.py "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.bin" --port COM9 --apply
```

**期待結果**:
- `OK: image staged and CRC-verified in X.Xs`
- `OK: Bank2 programmed and verified; NonSecure application launched`

**適用後、ボードは自動でシステムリセットして新イメージを起動する**（十数秒かかる）。

```bash
sleep 16
cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: **PASS**（OTAで書き込んだファームが正常に動いている）

### A-4. 連続ホットOTA（2回目の適用）

過去にHardFaultしたケース。2回続けて適用できることを確認する。

```bash
cd "$REPO/pc_side/status_monitor"
python -c "
import serial, time
with serial.Serial('COM9', 921600, timeout=0.2) as s:
    for _ in range(4):
        s.write(b'\x00'); time.sleep(0.3); s.read(8192)
"
python ota_update.py "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.bin" --port COM9 --apply
sleep 16

cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: 2回目も `Bank2 programmed and verified` で成功し、`verify_regression.py` が **PASS**

**FAILしたら**: OTA機構かSecureの初期化が壊れている。ロールバックして原因を調査すること。

### A-5. Wi-Fi TCP経由の動作確認（可能なら）

PCのモバイルホットスポットを起動できる環境なら実施する（無ければスキップしてよい）。

```bash
# ホットスポットを起動（SSID=U585-IOT02A / pass=u585iot02a）
cd "$REPO"
powershell -ExecutionPolicy Bypass -File pc_side/wifi_hotspot.ps1 -Action Start

# ボードをリセットしてWi-Fi接続させる
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
"$CLI" -c port=SWD mode=UR -rst
sleep 20

# ボードのIPを探す
powershell -ExecutionPolicy Bypass -File pc_side/find_board.ps1

# 見つかったIPでOTA状態を照会（例: 192.168.137.2）
cd pc_side/status_monitor
python ota_update.py --status --tcp 192.168.137.2:5000
```

**期待結果**: TCP経由で `OTA state : Idle` などの応答が返る（Wi-Fi/TCPスタックが生きている）

### A-6. AI推論とテストスイート（Secureローダーパス）

`App_Main()`（Bank2が空、またはUSERボタンを押しながら起動したときに動くローダー）が壊れていないか確認する。

**USERボタン(B1)を押しながらリセット**すると、SecureのOTAローダーに留まる。この状態で:

```bash
cd "$REPO"
python -c "
import sys, time
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
import serial
with serial.Serial('COM9', 921600, timeout=0.2) as s:
    s.write(b'i')   # AI推論を1回実行
    t0=time.time(); buf=b''
    while time.time()-t0 < 5:
        buf += s.read(8192)
    for line in buf.decode('utf-8', errors='replace').splitlines():
        if 'AI' in line or 'infer' in line:
            print(line)
"
```

**期待結果**: 推論結果のログが出る（`[AI] ...`）。これが出れば `Telemetry_GetAudioBuffer`（step5で `audio_capture.cpp` に移した）が正しく機能している。

**このテストが難しければスキップしてよい**が、その場合は「AI推論は未検証」とコミットメッセージに明記すること。

---

## パート B: 総合回帰チェックリスト

以下をすべて確認し、チェックを付ける。

- [ ] Secure/NonSecure両方がクリーンビルドで0エラー
- [ ] `verify_regression.py` が PASS（SWD書き込み後）
- [ ] OTA実ファーム適用が成功し、適用後も `verify_regression.py` が PASS
- [ ] 連続2回目のOTA適用も成功する
- [ ] （可能なら）Wi-Fi TCP経由でOTA状態照会が返る
- [ ] （可能なら）AI推論が動く
- [ ] 目視: ACTIVE中は緑LEDが250ms点滅、IDLE中は赤LEDが1秒点滅

**1つでも失敗したら、リファクタリングは完了していない。** 該当するステップに戻って原因を修正すること。

---

## パート C: ドキュメント更新

### C-1. `アーキテクチャ・メモリマップ.md` を更新する

「3. ソースファイルの配置」の表を、新しい構造に書き換える。

**Secure側の表を以下に差し替える**:

| ファイル | 役割 | レイヤ |
|---|---|---|
| `comm_service.cpp` | 通信サービス本体（テレメトリ収集・フレーム処理・OTA・NSCブリッジ） | コア層（基板非依存） |
| `comm_wifi.cpp` | EMW3080 Wi-Fi + TCPトランスポート | **port層** |
| `comm_ble.cpp` | STM32WB5MMG BLE（ATコマンド） | **port層** |
| `comm_uart.cpp` | VCP(USART1)非同期送信 | **port層** |
| `audio_capture.cpp` | MIC2/MDF1 DMAキャプチャ | **port層** |
| `mcu_info.cpp` | 内蔵ADC・メモリ統計 | チップ依存 |
| `console.cpp` | UART受信（循環DMA） | **port層** |
| `ota.cpp` | OTAステージング・Bank2適用・ロールバック | コア層 |
| `boot_guard.cpp` | 起動監視 | コア層 |
| `frame_codec.c` | フレームプロトコル | コア層 |
| `secure_nsc.c` | CMSEゲートウェイ | アダプタ層（TrustZone固有） |
| `main.c` | Stage-0ローダー・GTZC設定 | 基板依存 |

**NonSecure側の表を以下に差し替える**:

| ファイル | 役割 | レイヤ |
|---|---|---|
| `main.c` | 初期化して `App_Run()` を呼ぶだけ | 基板依存（薄い） |
| `app_loop.c` | ACTIVE/IDLEステートマシン | **コア層（基板非依存）** |
| `board_io.c` | LED・ボタンのGPIO | **port層** |
| `sensors.c` | センサー読み取り | **port層** |
| `ns_audio.c` | 音声のRMS/波形計算 | コア層 |

**「レイヤ構造」の節を新設して、以下を追記する**:

```markdown
## レイヤ構造（自作基板への移植のために）

| レイヤ | 意味 | 移植時 |
|---|---|---|
| 契約層 | ヘッダのみ。関数の名前と意味を決める | 変更しない |
| コア層 | ハードウェアに触らない。契約層のAPI経由でのみ動く | そのままコピー |
| port層 | ハードウェアに直接触る。基板固有 | **新基板用に書き直す** |
| アダプタ層 | TrustZoneの有無を吸収する | どちらかを選ぶ |

移植手順は `portability/PORTING.md` を参照。
```

### C-2. `UARTコマンド・LED状態リファレンス.md` の更新

**変更不要**（コマンドもLEDの意味も変わっていない）。

### C-3. `TrustZone リファクタリング計画.md` に完了記録を追記する

末尾に以下を追記:

```markdown
---

## レイヤ分離リファクタリング（2026-07-XX 完了）

自作基板への移行を見据え、「port層を差し替えれば別の基板で動く」構造へ再編した。

### 成果

| 項目 | Before | After |
|---|---|---|
| `telemetry.cpp` | 1417行（通信3系統+音声+MCU情報+OTA+NSCブリッジが同居） | `comm_service.cpp` 約450行（通信サービス本体のみ） |
| 分離されたport層 | なし | `comm_wifi.cpp` / `comm_ble.cpp` / `comm_uart.cpp` / `audio_capture.cpp` / `mcu_info.cpp` / `board_io.c` |
| NonSecureアプリ層 | main.cにGPIO直叩きとステートマシンが同居 | `app_loop.c`（基板非依存）+ `board_io.c`（port層）に分離 |
| TrustZone依存 | NonSecureが `secure_nsc.h` を直接include | `comm_api.h`（契約層）経由。TZの有無を知らない |
| 移植キット | なし | `portability/`（非TZ基板用アダプタ + PORTING.md） |

### 設計方針

- **リンク時差し替え**（関数ポインタのopsテーブルは使わない）: 契約=ヘッダ、実装=portファイル。実装漏れはリンクエラーで検出される
- **OTA信頼ルートは分離しない**: `handleFrame` / `processRxByte` / `ota::Manager` はcomm_service.cppに集約したまま。フレームバイトがport層に漏れない構造を維持
- **CPU負荷計算は分離しない**: Serviceの内部状態に強く依存するため

### 実施ステップ
`docs/refactoring/` に step0〜step8 の詳細な実行記録がある。

### 検証
全ステップで実機検証を実施。最終的に OTA実ファーム往復（連続2回）まで含めた総合回帰をPASS。
```

---

## パート D: ブランチのマージ

すべての検証がPASSしたら、masterへマージする。

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

# ドキュメント更新をコミット
git add "アーキテクチャ・メモリマップ.md" "TrustZone リファクタリング計画.md"
git commit -m "docs(step8): レイヤ分離リファクタリングの完了記録とファイル配置を更新"

# masterへマージ
git checkout master
git merge --no-ff refactor/layering -m "レイヤ分離リファクタリング完了: port層を差し替えれば別基板で動く構造へ

telemetry.cpp(1417行)を機能単位に分割し、基板依存コード(port層)を明確に分離した。
NonSecureのアプリ層はTrustZoneの有無を知らなくなり、非TZ基板へも無改造で移植できる。

全ステップで実機検証を実施し、OTA実ファーム往復(連続2回)を含む総合回帰をPASS。
移植手順は portability/PORTING.md を参照。"

# ベースラインタグは残しておく（いつでも戻れるように）
git tag
```

---

## 完了条件

- [ ] パートAの総合回帰テストが全項目PASS
- [ ] パートBのチェックリストが全部チェック済み
- [ ] ドキュメント3件を更新
- [ ] masterへマージ完了

## もし途中で失敗したら

**リファクタリング全体を捨てて元に戻す場合**:

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git checkout master           # refactor/layering は残る（後で調べられる）
git checkout refactor-baseline -- .   # 念のためベースラインのコードに戻す

# ビルドして書き込み、動作を確認する
```

ブランチ `refactor/layering` は消さずに残しておくこと。どこまで進んだかの記録になる。
