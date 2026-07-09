/**
  ******************************************************************************
  * @file    frame_codec.h
  * @brief   Command frame codec (requirement spec section 11.2).
  *          RTOS/transport independent - shared by UART and BLE paths.
  *
  *          +------+--------+--------+-------+---------+-------+------+
  *          | SOF  | CMD_ID | SEQ_NO | LEN   | PAYLOAD | CRC16 | EOF  |
  *          | 0xAA | 1byte  | 1byte  | 2byte | 0..1024 | 2byte | 0x55 |
  *          +------+--------+--------+-------+---------+-------+------+
  *          Little endian. CRC-16/CCITT-FALSE over SOF..PAYLOAD.
  ******************************************************************************
  */
#ifndef FRAME_CODEC_H
#define FRAME_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_SOF              0xAAU
#define FRAME_EOF              0x55U
#define FRAME_OVERHEAD         8U      /* SOF+CMD+SEQ+LEN(2)+CRC(2)+EOF */
#define FRAME_MAX_PAYLOAD      1024U

/* Command IDs (11.2 + telemetry extensions) */
#define FRAME_CMD_STATUS       0x01U   /* full telemetry (UART)              */
#define FRAME_CMD_STATUS_MINI  0x02U   /* compact telemetry (BLE notify)     */
#define FRAME_CMD_AUDIO        0x03U   /* PCM audio block (512 x int16, LE)  */

uint16_t Frame_Crc16(const uint8_t *data, size_t len);

/* Builds a frame into out (size >= payload_len + FRAME_OVERHEAD).
 * Returns the total frame length, or 0 if the payload is too large. */
size_t Frame_Encode(uint8_t cmd, uint8_t seq, const uint8_t *payload,
                    size_t payload_len, uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_CODEC_H */
