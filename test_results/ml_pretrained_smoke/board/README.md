# B-U585I-IOT02A YamNet 6-class on-board smoke test

Date: 2026-08-24 (JST)

## Safety and backup

- Board: B-U585I-IOT02A
- ST-LINK: `0026003C3432511630343838`, firmware V3J17M10
- STM32CubeProgrammer: 2.23.0
- Original 2 MiB flash backup (kept outside this repository):
  `D:\App\STM32CubeIDE\workspace_2.1.1\board_backups\20260824_before_yamnet6\device_flash_0x08000000_2MiB.bin`
- Backup size: 2,097,152 bytes
- Backup SHA-256: `7151c7027e9c11f46a056d75254000a966ae16887f36f9cbfebec9cd7fb4a533`
- Read-only option-byte check: RDP level 0 (`0xAA`), `TZEN=1`, Bank 1 secure, Bank 2 non-secure.
- No option bytes were changed. OTA was not used.

## Firmware build and placement

Both STM32CubeIDE 2.2.0 Debug builds completed successfully with the Secure
project registered as the NonSecure project's dependency.

| image | text | data | bss | ELF SHA-256 | load address |
|---|---:|---:|---:|---|---|
| Secure | 508,416 B | 8,636 B | 210,952 B | `4c90a3c642ac9f9c4b8935ac234fa471a7b2734a5e67d852644f59ec5eb87c9d` | `0x0c000000` |
| NonSecure | 138,916 B | 476 B | 19,452 B | `0d3c01a14374c68d56d3524a64d3110f10264c87e584688925bed7adcdeeb799` | `0x08100000` |

The Secure ELF contains the `audio_net` network, all six class labels, and the
`AI_RunOnce` non-secure-callable entry. The NonSecure ELF resolves its
`AI_RunOnce` veneer to `0x0c0fe030`.

Only the CMSIS-DSP FFT tables required by this model are linked:
`twiddleCoef_256`, `armBitRevIndexTable256`, and `twiddleCoef_rfft_512`.
This reduced Secure `text` by 115,936 bytes from the initial verified build.

## Programming and execution

- Final minimized Secure ELF download and verify: PASS
- Final NonSecure ELF download and verify: PASS
- Software reset after both verified images: PASS
- UART: ST-LINK VCP COM9 at 921600 baud
- Model initialization: PASS, 23,930,924 MACC, int8 input 6,144 values,
  float output 6 classes
- Ten consecutive ambient inferences with the minimized firmware: 10/10 PASS
- Preprocessing range: 119-120 ms
- Inference range: 279-280 ms
- Top class: `other` on all ten runs, confidence 94-98%

The test confirms capture of the approximately 0.975-second microphone window,
official YamNet preprocessing, TrustZone gateway execution, and stable
six-class inference. Controlled speech, knock, glass, crying, and gunshot test
stimuli were not played during this smoke test.

## Disk cleanup

After the verified board run, the external Model Zoo Services checkout (about
327 MB), temporary generated-code/build result trees (about 7.3 MB), and two
temporary CubeIDE workspaces (about 1.9 MB) were deleted. These were disposable
generated/downloaded data and are not recoverable locally without fetching or
rebuilding them. The minimal pinned Model Zoo checkout containing the exact
184,240-byte model and Git metadata remains on D: (about 0.70 MB).
