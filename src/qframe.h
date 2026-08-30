#pragma once
#include <stdint.h>
#include <stddef.h>

// ResMed UART frame format:
// [0x55] [Type] [Len:3hex] [Payload with 0x55 escaping] [CRC16:4hex]

#define QFRAME_SYNC         0x55
#define QFRAME_MAX_PAYLOAD  502
#define QFRAME_MAX_RAW      600  // maximum accepted escaped wire length

#define QFRAME_TYPE_E       'E'  // error response
#define QFRAME_TYPE_FLASH   'f'  // bootloader flash data (host -> device)
#define QFRAME_TYPE_K       'K'  // stored stream data response
#define QFRAME_TYPE_L       'L'  // live stream report / oximetry injection
#define QFRAME_TYPE_O       'O'  // patched bootloader data / immediate reset
#define QFRAME_TYPE_P       'P'  // bootloader progress / immediate reset
#define QFRAME_TYPE_Q       'Q'  // ASCII command (host -> device)
#define QFRAME_TYPE_R       'R'  // success response
#define QFRAME_TYPE_T       'T'  // reserved by AirSense firmware

typedef uint16_t qframe_type_mask_t;

#define QFRAME_MASK_E       ((qframe_type_mask_t)1u << 0)
#define QFRAME_MASK_FLASH   ((qframe_type_mask_t)1u << 1)
#define QFRAME_MASK_K       ((qframe_type_mask_t)1u << 2)
#define QFRAME_MASK_L       ((qframe_type_mask_t)1u << 3)
#define QFRAME_MASK_O       ((qframe_type_mask_t)1u << 4)
#define QFRAME_MASK_P       ((qframe_type_mask_t)1u << 5)
#define QFRAME_MASK_Q       ((qframe_type_mask_t)1u << 6)
#define QFRAME_MASK_R       ((qframe_type_mask_t)1u << 7)
#define QFRAME_MASK_T       ((qframe_type_mask_t)1u << 8)
#define QFRAME_MASK_ANY     ((qframe_type_mask_t)0x01ffu)


typedef enum {
    QFP_IDLE,
    QFP_TYPE,
    QFP_LEN0,
    QFP_LEN1,
    QFP_LEN2,
    QFP_PAYLOAD,
    QFP_PAYLOAD_ESC,
    QFP_CRC1,
    QFP_CRC2,
    QFP_CRC3,
    QFP_COMPLETE,
    QFP_ERROR,
} qframe_parse_state_t;

typedef struct {
    uint8_t     type;
    uint8_t     payload[QFRAME_MAX_PAYLOAD];
    uint16_t    payload_len;
    uint16_t    declared_len;
    uint16_t    crc_received;
    uint16_t    crc_computed;
    bool        crc_valid;
} qframe_t;

typedef struct {
    qframe_parse_state_t state;
    qframe_t    frame;
    uint16_t    raw_count;
    uint8_t     raw_buf[QFRAME_MAX_RAW];
    uint8_t     len_chars[3];
    uint8_t     crc_chars[4];
} qframe_parser_t;

void        qframe_parser_init(qframe_parser_t *p);
void        qframe_parser_reset(qframe_parser_t *p);

bool        qframe_parser_feed(qframe_parser_t *p, uint8_t byte);

const qframe_t* qframe_parser_frame(const qframe_parser_t *p);

int         qframe_build(uint8_t type, const uint8_t *payload, uint16_t payload_len,
                         uint8_t *out_buf, uint16_t out_buf_size);

int         qframe_build_cmd(const char *cmd, uint8_t *out_buf, uint16_t out_buf_size);

qframe_type_mask_t qframe_type_mask(uint8_t type);
const char         *qframe_type_name(uint8_t type);

int         hex_nibble(uint8_t c);
uint8_t     nibble_hex(uint8_t n);

const char *qframe_response_value(const char *resp);
