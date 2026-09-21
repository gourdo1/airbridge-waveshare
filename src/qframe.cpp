#include "qframe.h"
#include "crc.h"
#include <string.h>

int hex_nibble(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

uint8_t nibble_hex(uint8_t n) {
    return (n < 10) ? ('0' + n) : ('A' + n - 10);
}


void qframe_parser_init(qframe_parser_t *p) {
    qframe_parser_reset(p);
}

void qframe_parser_reset(qframe_parser_t *p) {
    memset(p, 0, sizeof(*p));
    p->state = QFP_IDLE;
    p->frame.crc_computed = 0xFFFF;
}

static void consume_wire_byte(qframe_parser_t *p, uint8_t byte) {
    p->frame.crc_computed = crc16_ccitt(&byte, 1, p->frame.crc_computed);
    p->raw_count++;
}

const qframe_t* qframe_parser_frame(const qframe_parser_t *p) {
    return (p->state == QFP_COMPLETE) ? &p->frame : NULL;
}

bool qframe_parser_feed(qframe_parser_t *p, uint8_t byte) {
    if ((p->state == QFP_COMPLETE || p->state == QFP_ERROR) && byte == QFRAME_SYNC) {
        qframe_parser_reset(p);
    }

    switch (p->state) {
    case QFP_IDLE:
        if (byte == QFRAME_SYNC) {
            qframe_parser_reset(p);
            consume_wire_byte(p, byte);
            p->state = QFP_TYPE;
        }
        break;

    case QFP_TYPE:
        p->frame.type = byte;
        consume_wire_byte(p, byte);
        p->state = QFP_LEN0;
        break;

    case QFP_LEN0:
        p->len_chars[0] = byte;
        consume_wire_byte(p, byte);
        p->state = QFP_LEN1;
        break;

    case QFP_LEN1:
        p->len_chars[1] = byte;
        consume_wire_byte(p, byte);
        p->state = QFP_LEN2;
        break;

    case QFP_LEN2: {
        p->len_chars[2] = byte;
        consume_wire_byte(p, byte);

        int n0 = hex_nibble(p->len_chars[0]);
        int n1 = hex_nibble(p->len_chars[1]);
        int n2 = hex_nibble(p->len_chars[2]);
        if (n0 < 0 || n1 < 0 || n2 < 0) {
            p->state = QFP_ERROR;
            break;
        }
        p->frame.declared_len = (n0 << 8) | (n1 << 4) | n2;

        if (p->frame.declared_len < 9 || p->frame.declared_len > QFRAME_MAX_RAW) {
            p->state = QFP_ERROR;
            break;
        }
        p->frame.payload_len = 0;
        p->state = QFP_PAYLOAD;
        break;
    }

    case QFP_PAYLOAD:
        if (p->raw_count >= (p->frame.declared_len - 4)) {
            p->crc_chars[0] = byte;
            p->state = QFP_CRC1;
        } else if (byte == QFRAME_SYNC) {
            consume_wire_byte(p, byte);
            p->state = QFP_PAYLOAD_ESC;
        } else {
            consume_wire_byte(p, byte);
            if (p->frame.payload_len < QFRAME_MAX_PAYLOAD) {
                p->frame.payload[p->frame.payload_len++] = byte;
            }
        }
        break;

    case QFP_PAYLOAD_ESC:
        if (byte == QFRAME_SYNC) {
            consume_wire_byte(p, byte);
            if (p->frame.payload_len < QFRAME_MAX_PAYLOAD) {
                p->frame.payload[p->frame.payload_len++] = QFRAME_SYNC;
            }
            p->state = QFP_PAYLOAD;
        } else {
            // Restart with the first 0x55 as new sync
            qframe_parser_reset(p);
            consume_wire_byte(p, QFRAME_SYNC);
            p->state = QFP_TYPE;
            return qframe_parser_feed(p, byte);
        }
        break;

    case QFP_CRC1:
        p->crc_chars[1] = byte;
        p->state = QFP_CRC2;
        break;

    case QFP_CRC2:
        p->crc_chars[2] = byte;
        p->state = QFP_CRC3;
        break;

    case QFP_CRC3: {
        p->crc_chars[3] = byte;

        int c0 = hex_nibble(p->crc_chars[0]);
        int c1 = hex_nibble(p->crc_chars[1]);
        int c2 = hex_nibble(p->crc_chars[2]);
        int c3 = hex_nibble(p->crc_chars[3]);
        if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) {
            p->state = QFP_ERROR;
            break;
        }
        p->frame.crc_received = (c0 << 12) | (c1 << 8) | (c2 << 4) | c3;
        p->frame.crc_valid = (p->frame.crc_received == p->frame.crc_computed);
        p->state = QFP_COMPLETE;
        return true;
    }

    case QFP_COMPLETE:
    case QFP_ERROR:
        break;
    }

    return false;
}


int qframe_build(uint8_t type, const uint8_t *payload, uint16_t payload_len,
                 uint8_t *out_buf, uint16_t out_buf_size)
{
    if (payload_len > QFRAME_MAX_PAYLOAD) return -1;

    uint16_t esc_len = 0;
    for (uint16_t i = 0; i < payload_len; i++) {
        esc_len += (payload[i] == QFRAME_SYNC) ? 2 : 1;
    }

    uint16_t total = 1 + 1 + 3 + esc_len + 4;
    if (total > out_buf_size || total > 0xFFF) return -1;

    uint16_t pos = 0;

    out_buf[pos++] = QFRAME_SYNC;

    out_buf[pos++] = type;

    out_buf[pos++] = nibble_hex((total >> 8) & 0xF);
    out_buf[pos++] = nibble_hex((total >> 4) & 0xF);
    out_buf[pos++] = nibble_hex(total & 0xF);

    for (uint16_t i = 0; i < payload_len; i++) {
        out_buf[pos++] = payload[i];
        if (payload[i] == QFRAME_SYNC) {
            out_buf[pos++] = QFRAME_SYNC;
        }
    }

    uint16_t crc = crc16_ccitt(out_buf, pos);
    out_buf[pos++] = nibble_hex((crc >> 12) & 0xF);
    out_buf[pos++] = nibble_hex((crc >> 8) & 0xF);
    out_buf[pos++] = nibble_hex((crc >> 4) & 0xF);
    out_buf[pos++] = nibble_hex(crc & 0xF);

    return (int)pos;
}

int qframe_build_cmd(const char *cmd, uint8_t *out_buf, uint16_t out_buf_size) {
    return qframe_build(QFRAME_TYPE_Q, (const uint8_t *)cmd, strlen(cmd),
                        out_buf, out_buf_size);
}

qframe_type_mask_t qframe_type_mask(uint8_t type) {
    switch (type) {
        case QFRAME_TYPE_E:     return QFRAME_MASK_E;
        case QFRAME_TYPE_FLASH: return QFRAME_MASK_FLASH;
        case QFRAME_TYPE_K:     return QFRAME_MASK_K;
        case QFRAME_TYPE_L:     return QFRAME_MASK_L;
        case QFRAME_TYPE_O:     return QFRAME_MASK_O;
        case QFRAME_TYPE_P:     return QFRAME_MASK_P;
        case QFRAME_TYPE_Q:     return QFRAME_MASK_Q;
        case QFRAME_TYPE_R:     return QFRAME_MASK_R;
        case QFRAME_TYPE_T:     return QFRAME_MASK_T;
        default:                return 0;
    }
}

const char *qframe_type_name(uint8_t type) {
    switch (type) {
        case QFRAME_TYPE_E:     return "error";
        case QFRAME_TYPE_FLASH: return "flash";
        case QFRAME_TYPE_K:     return "stored";
        case QFRAME_TYPE_L:     return "live";
        case QFRAME_TYPE_O:     return "dump/reset";
        case QFRAME_TYPE_P:     return "progress/reset";
        case QFRAME_TYPE_Q:     return "command";
        case QFRAME_TYPE_R:     return "response";
        case QFRAME_TYPE_T:     return "reserved";
        default:                return "unknown";
    }
}

const char *qframe_response_value(const char *resp) {
    if (!resp) return NULL;
    const char *eq = strstr(resp, "= ");
    return eq ? eq + 2 : NULL;
}

bool qframe_response_matches(const uint8_t *request, size_t request_len,
                              const qframe_t &response) {
    if (!request || request_len < 9 || request[1] != QFRAME_TYPE_Q)
        return true;
    if (response.type != QFRAME_TYPE_R && response.type != QFRAME_TYPE_E)
        return true;

    const size_t end = request_len - 4;
    bool registry = end >= 13 && memcmp(request + 5, "G C &CSG", 8) == 0 &&
                    (end == 13 || request[13] == ' ');
    size_t echo_len = 0;
    while (echo_len < response.payload_len && response.payload[echo_len] != '=')
        echo_len++;
    // Bare errors and stored frames carry no command identity.
    if (echo_len == response.payload_len) return !registry;
    while (echo_len && response.payload[echo_len - 1] == ' ') echo_len--;
    if (!echo_len) return false;

    size_t pos = 5;
    for (size_t i = 0; i < echo_len; i++) {
        if (pos >= end || request[pos++] != response.payload[i]) return false;
        if (response.payload[i] == QFRAME_SYNC) {
            if (pos >= end || request[pos++] != QFRAME_SYNC) return false;
        }
    }
    if (registry) return pos == end;
    // CDX setters echo the command and variable, but omit the input value.
    // Some capability replies omit their index too. Match any echoed args,
    // without requiring firmware to echo arguments it never sends.
    return pos == end || request[pos] == ' ';
}
