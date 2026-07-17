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
#define FRAME_CMD_STATUS       0x01U   /* full telemetry (UART/TCP)          */
#define FRAME_CMD_STATUS_MINI  0x02U   /* compact telemetry (BLE notify)     */
#define FRAME_CMD_AUDIO        0x03U   /* PCM audio block (512 x int16, LE)  */
#define FRAME_CMD_FW_CHUNK     0x04U   /* PC->board: offset u32 + data       */
#define FRAME_CMD_FW_COMPLETE  0x05U   /* PC->board: size u32 + crc16 u16    */
#define FRAME_CMD_STATUS_REQ   0x06U   /* PC->board: query OTA state         */
#define FRAME_CMD_STATUS_RESP  0x07U   /* board->PC: OTA state report        */
#define FRAME_CMD_FW_APPLY     0x08U   /* copy staged NS image to Bank2/run */
#define FRAME_CMD_REC_START    0x09U   /* PC->board: start SRAM recording    */
#define FRAME_CMD_REC_STOP     0x0AU   /* PC->board: stop SRAM recording     */
#define FRAME_CMD_REC_CHUNK    0x0BU   /* board->PC: raw TLV, not this codec */
#define FRAME_CMD_REC_END      0x0CU   /* board->PC: raw TLV, not this codec */
#define FRAME_CMD_LINK_STANDBY 0x0DU   /* PC->board: end/idle the sending link */
#define FRAME_CMD_TIME_SYNC    0x0EU   /* PC->board: u32 Unix epoch seconds   */
#define FRAME_CMD_ENTER_COMM   0x0FU   /* PC->board: explicit IDLE->ACTIVE wake */
#define FRAME_CMD_STOP_COMM    0x10U   /* PC->board: explicit ACTIVE->IDLE hint */
#define FRAME_CMD_LOG_REQ      0x11U   /* PC->board: u32 startIndex (paged log read) */
#define FRAME_CMD_LOG_RESP     0x12U   /* board->PC: u32 startIndex + u16 count + records */
#define FRAME_CMD_LOG_RESET    0x13U   /* PC->board: erase the non-volatile state log */
#define FRAME_CMD_IDLE_BEACON  0x14U   /* board->PC: alive-but-idle ping, no sensor data */
#define FRAME_CMD_ACK          0x7EU   /* {orig_cmd, orig_seq, u32 arg}      */
#define FRAME_CMD_NACK         0x7FU   /* {orig_cmd, orig_seq, u8 error}     */

/* NACK error codes */
#define FRAME_ERR_BAD_OFFSET   1U
#define FRAME_ERR_ERASE        2U
#define FRAME_ERR_WRITE        3U
#define FRAME_ERR_VERIFY       4U
#define FRAME_ERR_TOO_LARGE    5U
#define FRAME_ERR_BAD_STATE    6U

/* Streaming decoder for inbound frames. Bytes that are not part of a frame
 * are reported back to the caller so single-character console commands keep
 * working on the same link. */
typedef struct
{
  uint8_t buf[FRAME_MAX_PAYLOAD + FRAME_OVERHEAD];
  uint16_t pos;
} Frame_Decoder;

typedef enum
{
  FRAME_FEED_CONSUMED = 0, /* byte buffered, no event yet                */
  FRAME_FEED_COMPLETE = 1, /* full valid frame: out params are set       */
  FRAME_FEED_PLAIN = 2     /* byte is not frame data - treat as console  */
} Frame_FeedResult;

void Frame_DecoderInit(Frame_Decoder *dec);
Frame_FeedResult Frame_DecoderFeed(Frame_Decoder *dec, uint8_t byte,
                                   uint8_t *cmd, uint8_t *seq,
                                   const uint8_t **payload, uint16_t *len);

uint16_t Frame_Crc16(const uint8_t *data, size_t len);

/* Builds a frame into out (size >= payload_len + FRAME_OVERHEAD).
 * Returns the total frame length, or 0 if the payload is too large. */
size_t Frame_Encode(uint8_t cmd, uint8_t seq, const uint8_t *payload,
                    size_t payload_len, uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_CODEC_H */
