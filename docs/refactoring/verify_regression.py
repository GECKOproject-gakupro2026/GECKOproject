"""回帰検証スクリプト

使い方:
    cd <REPO>
    python docs/refactoring/verify_regression.py

前提:
    - ボードにSecure/NonSecureを書き込み、リセット済み
    - リセットから12秒以上経過している（Wi-Fi/BLE初期化の完了待ち）
    - ST-LINK VCPがCOM9に見えている（違う場合は下の PORT を書き換える）

最後に PASS / FAIL を印字する。1つでもFAILなら、その変更は実機で壊れて
いるので、コミットせずロールバックすること。

IDLE判定について(P1でIDLE再定義に合わせて改訂):
    IDLE状態は「トリガー待機状態」であり、通信は沈黙させない。電力を食う
    ToFだけSLEEPさせ、テレメトリ/BLE/TCPは低頻度で流し続ける設計。
    そのためIDLE遷移の判定は「フレームが止まる」ではなく「通信は継続し
    つつToFがSLEEPしている(tof_ok=0)」、IDLE復帰の判定は「フレーム再開」
    に加えて「ToFが再びranging中(tof_ok=1)」を見る。
"""
from __future__ import annotations

import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, r"d:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A\pc_side\status_monitor")

import serial  # noqa: E402
import protocol  # noqa: E402

PORT = "COM9"
BAUD = 921600

results: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str) -> None:
    results.append((name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def collect(ser, parser, secs: float, keepalive: bool):
    """secs秒間受信する。keepaliveがTrueなら1秒ごとに\\x00を送る。
    戻り値: (statusフレーム数, audioフレーム数, 最後のStatus, tof_mmの異なり数,
             tof_okがTrueだったフレーム数, tof_okがFalseだったフレーム数)"""
    t_end = time.time() + secs
    next_ka = time.time()
    n_status = n_audio = 0
    last = None
    tofs: list[int] = []
    tof_ok_count = tof_dead_count = 0
    while time.time() < t_end:
        if keepalive and time.time() >= next_ka:
            ser.write(b"\x00")
            next_ka += 1.0
        for item in parser.feed(ser.read(16384)):
            if item[0] != "frame":
                continue
            if item[1] == protocol.CMD_AUDIO:
                n_audio += 1
                continue
            st = protocol.decode_status(item[1], item[3])
            if st is not None and not st.compact:
                n_status += 1
                last = st
                if not tofs or tofs[-1] != st.tof_mm:
                    tofs.append(st.tof_mm)
                if st.tof_ok:
                    tof_ok_count += 1
                else:
                    tof_dead_count += 1
        time.sleep(0.02)
    return n_status, n_audio, last, len(tofs), tof_ok_count, tof_dead_count


def main() -> int:
    parser = protocol.FrameParser()
    with serial.Serial(PORT, BAUD, timeout=0.2) as ser:
        # 1) ACTIVE維持 + センサー生存
        print("1) ACTIVE維持とセンサー(5秒、keep-alive送信)")
        n_st, _, last, n_tof, _, _ = collect(ser, parser, 5.0, keepalive=True)
        check("ACTIVE維持", n_st >= 150, f"{n_st}フレーム受信 (要 >=150)")
        if last is not None:
            check("ToF生存", n_tof >= 2,
                  f"tof_mmが{n_tof}種類 (要 >=2。1種類=固定値=センサー死亡)")
            check("温度センサー", 10.0 <= last.temp_c <= 60.0,
                  f"temp_c={last.temp_c:.1f} (要 10〜60)")
        else:
            check("ToF生存", False, "Statusフレームが1つも来ない")
            check("温度センサー", False, "Statusフレームが1つも来ない")

        # 2) IDLE遷移
        # IDLEはもはや通信を沈黙させない(ToFのみSLEEPし、通信は生かしたまま
        # トリガーを待つ)。よって判定は「フレームが止まる」ではなく
        # 「通信は継続しつつToFがSLEEPしている(tof_ok=0)」に変わる。
        print("2) IDLE遷移(5秒間 keep-alive停止、IDLE中もテレメトリは継続)")
        n_st, _, _, _, _, _ = collect(ser, parser, 5.0, keepalive=False)
        # 最初の3秒はACTIVEなのでフレームが来る。IDLE突入後の状態を見たいので
        # 追加で2秒、無通信のまま観測する(IDLE中もkeep-alive無しでテレメトリは来る)
        n_idle, _, _, _, tof_ok_n, tof_dead_n = collect(
            ser, parser, 2.0, keepalive=False)
        check("IDLE遷移(通信継続)", n_idle > 0,
              f"無通信2秒後も{n_idle}フレーム受信 (要 >0。IDLE中も通信は生存する設計)")
        check("IDLE遷移(ToF SLEEP)", tof_dead_n > 0 and tof_ok_n == 0,
              f"tof_ok=True:{tof_ok_n}件 / False:{tof_dead_n}件 "
              f"(要 全てFalse。IDLE中はToFがSLEEPしtof_ok=0のはず)")

        # 3) IDLE復帰
        print("3) IDLE復帰(8秒、keep-alive再開)")
        n_st, _, _, _, tof_ok_n, _ = collect(ser, parser, 8.0, keepalive=True)
        check("IDLE復帰(フレーム再開)", n_st >= 100, f"{n_st}フレーム受信 (要 >=100)")
        check("IDLE復帰(ToF WAKE)", tof_ok_n > 0,
              f"tof_ok=Trueが{tof_ok_n}件 (要 >0。復帰後ToFが再びrangingしているはず)")

        # 4) 音声ストリーミング
        print("4) 音声ストリーミング('a'で開始、's'で停止)")
        ser.write(b"a")
        _, n_audio, _, _, _, _ = collect(ser, parser, 3.0, keepalive=True)
        check("音声ストリーム開始", n_audio >= 50,
              f"CMD_AUDIOを{n_audio}フレーム受信 (要 >=50)")
        ser.write(b"s")
        _, n_audio_off, _, _, _, _ = collect(ser, parser, 2.0, keepalive=True)
        check("音声ストリーム停止", n_audio_off <= 5,
              f"停止後 {n_audio_off}フレーム (要 <=5。惰性分の許容)")

        # 5) OTA照会
        print("5) OTA状態照会(CMD_STATUS_REQ -> CMD_STATUS_RESP)")
        ser.write(b"\x00")  # 念のため起こす
        time.sleep(0.2)
        ser.write(protocol.build_frame(protocol.CMD_STATUS_REQ, 0, b""))
        got_resp = False
        t_end = time.time() + 4.0
        while time.time() < t_end and not got_resp:
            for item in parser.feed(ser.read(16384)):
                if item[0] == "frame" and item[1] == protocol.CMD_STATUS_RESP:
                    got_resp = True
        check("OTA照会応答", got_resp, "STATUS_RESP受信" if got_resp else "応答なし")

    print()
    n_fail = sum(1 for _, ok, _ in results if not ok)
    if n_fail == 0:
        print("=== PASS: 全項目OK。コミットしてよい ===")
        return 0
    print(f"=== FAIL: {n_fail}項目が不合格。コミットせずロールバックすること ===")
    for name, ok, detail in results:
        if not ok:
            print(f"    - {name}: {detail}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
