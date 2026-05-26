#include "uart_arbiter.h"
#include "app_config.h"
#include "debug_log.h"
#include "live_stream.h"
#include <freertos/queue.h>

#define ARBITER_QUEUE_DEPTH     8
#define ARBITER_TASK_STACK      4096
#define ARBITER_TASK_PRIO       5
#define RX_TASK_STACK           3072
#define RX_TASK_PRIO            6
#define RX_BUF_SIZE             1024
#define RX_FRAME_QUEUE_DEPTH    4

static HardwareSerial *uart = nullptr;
static TaskHandle_t arbiter_task_handle = nullptr;
static TaskHandle_t rx_task_handle = nullptr;

static SemaphoreHandle_t cleanup_mutex = nullptr;
static SemaphoreHandle_t rx_ready = nullptr;

static volatile system_state_t sys_state = SYS_IDLE;
static volatile uart_ticket_t *current_ticket = nullptr;
static qframe_parser_t rx_parser;

static portMUX_TYPE rx_frame_mux = portMUX_INITIALIZER_UNLOCKED;
static qframe_t rx_frame_storage[RX_FRAME_QUEUE_DEPTH];
static uint8_t rx_frame_queue[RX_FRAME_QUEUE_DEPTH];
static uint8_t rx_frame_slot_state[RX_FRAME_QUEUE_DEPTH];
static uint8_t rx_frame_head = 0;
static uint8_t rx_frame_tail = 0;
static uint8_t rx_frame_count = 0;
static uint32_t rx_frame_epoch = 0;

static volatile bool transparent_active = false;
static Stream *transparent_bridge = nullptr;
static volatile uint32_t transparent_last_activity = 0;
static qframe_parser_t transparent_parser;      // shadow parser for BDD sniffing

static uint32_t current_baud = 0;

static uint32_t stat_tx = 0;
static uint32_t stat_rx = 0;
static uint32_t stat_l_rx = 0;
static uint32_t stat_timeout = 0;
static uint32_t stat_error = 0;

static uint32_t next_ticket_id = 1;


typedef struct {
    uart_ticket_t *tickets[ARBITER_QUEUE_DEPTH];
    int count;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t available;
} prio_queue_t;

static prio_queue_t pq;

static void pq_init() {
    pq.count = 0;
    pq.mutex = xSemaphoreCreateMutex();
    pq.available = xSemaphoreCreateCounting(ARBITER_QUEUE_DEPTH, 0);
}

static bool pq_push(uart_ticket_t *t) {
    xSemaphoreTake(pq.mutex, portMAX_DELAY);
    if (pq.count >= ARBITER_QUEUE_DEPTH) {
        xSemaphoreGive(pq.mutex);
        return false;
    }
    pq.tickets[pq.count++] = t;
    xSemaphoreGive(pq.mutex);
    xSemaphoreGive(pq.available);
    return true;
}

static uart_ticket_t* pq_pop(TickType_t wait) {
    if (xSemaphoreTake(pq.available, wait) != pdTRUE) {
        return nullptr;
    }
    xSemaphoreTake(pq.mutex, portMAX_DELAY);

    // highest prio value wins, then FIFO by ticket_id
    int best = 0;
    for (int i = 1; i < pq.count; i++) {
        if (pq.tickets[i]->priority > pq.tickets[best]->priority ||
            (pq.tickets[i]->priority == pq.tickets[best]->priority &&
             pq.tickets[i]->ticket_id < pq.tickets[best]->ticket_id)) {
            best = i;
        }
    }
    uart_ticket_t *t = pq.tickets[best];
    // Remove by shifting
    for (int i = best; i < pq.count - 1; i++) {
        pq.tickets[i] = pq.tickets[i + 1];
    }
    pq.count--;
    xSemaphoreGive(pq.mutex);
    return t;
}

static uint32_t parse_bdd_baud(const uint8_t *payload, uint16_t len);

typedef enum {
    RX_SLOT_FREE,
    RX_SLOT_RESERVED,
    RX_SLOT_QUEUED,
} rx_slot_state_t;

typedef enum {
    RX_PUSH_STORED,
    RX_PUSH_STORED_DROPPED_OLD,
    RX_PUSH_DROPPED_FULL,
    RX_PUSH_DROPPED_STALE,
} rx_push_result_t;

static int rx_frame_priority(uint8_t type) {
    switch (type) {
        case QFRAME_TYPE_E: return 3;
        case QFRAME_TYPE_R: return 2;
        default: return 1;
    }
}

static uint8_t rx_queue_slot(uint8_t logical_index) {
    return (rx_frame_tail + logical_index) % RX_FRAME_QUEUE_DEPTH;
}

static int rx_find_free_slot() {
    for (uint8_t i = 0; i < RX_FRAME_QUEUE_DEPTH; i++) {
        if (rx_frame_slot_state[i] == RX_SLOT_FREE) {
            return i;
        }
    }
    return -1;
}

static uint8_t rx_queue_clear_locked() {
    uint8_t cleared = rx_frame_count;
    for (uint8_t i = 0; i < rx_frame_count; i++) {
        uint8_t slot = rx_frame_queue[rx_queue_slot(i)];
        rx_frame_slot_state[slot] = RX_SLOT_FREE;
    }
    rx_frame_head = 0;
    rx_frame_tail = 0;
    rx_frame_count = 0;
    rx_frame_epoch++;
    return cleared;
}

static uint8_t rx_queue_drop_at(uint8_t logical_index) {
    uint8_t dropped_slot = rx_frame_queue[rx_queue_slot(logical_index)];

    for (uint8_t i = logical_index; i + 1 < rx_frame_count; i++) {
        rx_frame_queue[rx_queue_slot(i)] = rx_frame_queue[rx_queue_slot(i + 1)];
    }
    rx_frame_head = (rx_frame_head + RX_FRAME_QUEUE_DEPTH - 1) % RX_FRAME_QUEUE_DEPTH;
    rx_frame_count--;

    return dropped_slot;
}

static rx_push_result_t rx_queue_push(const qframe_t *frame) {
    bool overflow = false;
    uint8_t slot = 0;
    uint32_t epoch = 0;

    portENTER_CRITICAL(&rx_frame_mux);
    if (rx_frame_count >= RX_FRAME_QUEUE_DEPTH) {
        int new_prio = rx_frame_priority(frame->type);
        uint8_t drop_index = 0;
        bool found_lower_priority = false;

        for (uint8_t i = 0; i < rx_frame_count; i++) {
            uint8_t queued_slot = rx_frame_queue[rx_queue_slot(i)];
            if (rx_frame_priority(rx_frame_storage[queued_slot].type) < new_prio) {
                drop_index = i;
                found_lower_priority = true;
                break;
            }
        }

        if (!found_lower_priority) {
            portEXIT_CRITICAL(&rx_frame_mux);
            return RX_PUSH_DROPPED_FULL;
        }

        slot = rx_queue_drop_at(drop_index);
        overflow = true;
    } else {
        int free_slot = rx_find_free_slot();
        if (free_slot < 0) {
            portEXIT_CRITICAL(&rx_frame_mux);
            return RX_PUSH_DROPPED_FULL;
        }
        slot = (uint8_t)free_slot;
    }

    rx_frame_slot_state[slot] = RX_SLOT_RESERVED;
    epoch = rx_frame_epoch;
    portEXIT_CRITICAL(&rx_frame_mux);

    memcpy(&rx_frame_storage[slot], frame, sizeof(qframe_t));

    portENTER_CRITICAL(&rx_frame_mux);
    if (epoch != rx_frame_epoch) {
        rx_frame_slot_state[slot] = RX_SLOT_FREE;
        portEXIT_CRITICAL(&rx_frame_mux);
        return RX_PUSH_DROPPED_STALE;
    }
    rx_frame_queue[rx_frame_head] = slot;
    rx_frame_head = (rx_frame_head + 1) % RX_FRAME_QUEUE_DEPTH;
    rx_frame_count++;
    rx_frame_slot_state[slot] = RX_SLOT_QUEUED;
    portEXIT_CRITICAL(&rx_frame_mux);

    return overflow ? RX_PUSH_STORED_DROPPED_OLD : RX_PUSH_STORED;
}

static bool rx_queue_pop(qframe_t *out) {
    bool ok = false;

    portENTER_CRITICAL(&rx_frame_mux);
    if (rx_frame_count > 0) {
        uint8_t slot = rx_frame_queue[rx_frame_tail];
        rx_frame_tail = (rx_frame_tail + 1) % RX_FRAME_QUEUE_DEPTH;
        rx_frame_count--;
        if (out) memcpy(out, &rx_frame_storage[slot], sizeof(qframe_t));
        rx_frame_slot_state[slot] = RX_SLOT_FREE;
        ok = true;
    }
    portEXIT_CRITICAL(&rx_frame_mux);

    return ok;
}


static void rx_task(void *param) {
    uint8_t buf[64];
    qframe_parser_init(&rx_parser);

    while (true) {
        if (transparent_active) {
            // In transparent mode, forward raw bytes to bridge
            // Also feed a shadow parser to detect BDD R-frame responses
            int avail = uart->available();
            if (avail > 0) {
                int n = uart->readBytes(buf, min(avail, (int)sizeof(buf)));
                transparent_last_activity = millis();
                Log::logf(CAT_ARB, LOG_DEBUG, "[ARB] TRANSP RX %d bytes t=%lu\n", n, millis());
                if (transparent_bridge && n > 0) {
                    transparent_bridge->write(buf, n);
                }
                // Sniff for BDD R-frame
                for (int i = 0; i < n; i++) {
                    if (qframe_parser_feed(&transparent_parser, buf[i])) {
                        const qframe_t *f = qframe_parser_frame(&transparent_parser);
                        if (f && f->crc_valid) {
                            char pl[48] = {};
                            int plen = f->payload_len < sizeof(pl)-1 ? f->payload_len : sizeof(pl)-1;
                            memcpy(pl, f->payload, plen);
                            Log::logf(CAT_ARB, LOG_DEBUG, "[ARB] Shadow type=%c len=%u: %s\n",
                                      (char)f->type, f->payload_len, pl);
                        } else {
                            Log::logf(CAT_ARB, LOG_DEBUG, "[ARB] Shadow type=%c len=%u crc=BAD\n",
                                      f ? (char)f->type : '?', f ? f->payload_len : 0);
                        }
                        if (f && f->crc_valid && f->type == QFRAME_TYPE_R) {
                            uint32_t new_baud = parse_bdd_baud(f->payload, f->payload_len);
                            if (new_baud && new_baud != current_baud) {
                                if (transparent_bridge) transparent_bridge->flush();
                                vTaskDelay(pdMS_TO_TICKS(10));
                                uart->updateBaudRate(new_baud);
                                Log::logf(CAT_ARB, LOG_INFO, "[ARB] BDD transparent: baud %u -> %u\n",
                                          current_baud, new_baud);
                                current_baud = new_baud;
                            }
                        }
                        qframe_parser_reset(&transparent_parser);
                    }
                }
            } else {
                vTaskDelay(1);
            }
            continue;
        }

        int avail = uart->available();
        if (avail <= 0) {
            vTaskDelay(1);
            continue;
        }

        int n = uart->readBytes(buf, min(avail, (int)sizeof(buf)));
        for (int i = 0; i < n; i++) {
            if (qframe_parser_feed(&rx_parser, buf[i])) {
                // Complete frame
                const qframe_t *f = qframe_parser_frame(&rx_parser);
                if (f && f->crc_valid) {
                    if (f->type == QFRAME_TYPE_L) {
                        // Unsolicited live stream sample. Route to LiveStream
                        stat_rx++;
                        stat_l_rx++;
                        Log::logf(CAT_ARB, LOG_DEBUG,
                                  "[ARB] RX-L tag=%c%c%c len=%u t=%lu\n",
                                  f->payload_len > 0 ? (char)f->payload[0] : '?',
                                  f->payload_len > 1 ? (char)f->payload[1] : '?',
                                  f->payload_len > 2 ? (char)f->payload[2] : '?',
                                  f->payload_len, millis());
                        LiveStream::on_l_frame(f->payload, f->payload_len);
                    } else {
                        stat_rx++;
                        rx_push_result_t push_result = rx_queue_push(f);
                        if (push_result == RX_PUSH_STORED ||
                            push_result == RX_PUSH_STORED_DROPPED_OLD) {
                            if (rx_ready) {
                                xSemaphoreGive(rx_ready);
                            }
                        }
                        if (push_result == RX_PUSH_STORED_DROPPED_OLD) {
                            stat_error++;
                            Log::logf(CAT_ARB, LOG_WARN,
                                      "[ARB] RX queue full, dropped queued frame t=%lu\n",
                                      millis());
                        } else if (push_result == RX_PUSH_DROPPED_FULL) {
                            stat_error++;
                            Log::logf(CAT_ARB, LOG_WARN,
                                      "[ARB] RX queue full, dropped incoming frame t=%lu\n",
                                      millis());
                        }
                    }
                } else {
                    stat_error++;
                    Log::logf(CAT_ARB, LOG_WARN, "[ARB] RX frame CRC error t=%lu\n", millis());
                }
                qframe_parser_reset(&rx_parser);
            }
        }
    }
}


static void lcd_check();

static void arbiter_task(void *param) {
    while (true) {
        uart_ticket_t *t = pq_pop(pdMS_TO_TICKS(100));
        if (!t) {
            lcd_check();
            continue;
        }

        // Send frame
        current_ticket = t;
        if (!t->no_ack) Arbiter::clear_rx_frames();

        uart->write(t->frame, t->frame_len);
        uart->flush();
        stat_tx++;
        {
            // payload starts at offset=5
            char snip[33] = {};
            int plen = t->frame_len > 5 ? t->frame_len - 9 : 0;  // minus header(5)+crc(4)
            if (plen > 0) memcpy(snip, t->frame + 5, min(plen, 32));
            Log::logf(CAT_ARB, LOG_DEBUG, "[ARB] TX %s src=%d prio=%d t=%lu\n",
                      snip, t->source, t->priority, millis());
        }

        if (t->no_ack) {
            t->success = true;
            t->timed_out = false;
            t->resp_len = 0;
        } else {
            qframe_t rx;
            bool got_rx = Arbiter::wait_frame(&rx, t->timeout_ms);
            if (got_rx) {
                // Got response
                t->resp_type = rx.type;
                t->resp_len = rx.payload_len;
                if (t->resp_len > 0) {
                    memcpy(t->resp_payload, rx.payload,
                           min((int)t->resp_len, (int)sizeof(t->resp_payload)));
                }
                t->success = (rx.type == QFRAME_TYPE_R);
                t->timed_out = false;
                {
                    char snip[33] = {};
                    if (t->resp_len > 0) memcpy(snip, t->resp_payload, min((int)t->resp_len, 32));
                    Log::logf(CAT_ARB, LOG_DEBUG, "[ARB] RX %s %s t=%lu\n",
                              snip, t->success ? "ok" : "err", millis());
                }
                if (rx.type == QFRAME_TYPE_E) {
                    stat_error++;
                }
            } else {
                t->success = false;
                t->timed_out = true;
                t->resp_len = 0;
                stat_timeout++;
                Log::logf(CAT_ARB, LOG_DEBUG, "[ARB] RX timeout after %dms src=%d t=%lu\n",
                          t->timeout_ms, t->source, millis());
            }
        }

        current_ticket = nullptr;

        if (t->no_ack) {
            // send_frame: arbiter is sole owner of the heap ticket.
            free(t);
            continue;
        }

        xSemaphoreTake(cleanup_mutex, portMAX_DELAY);
        if (t->cancelled) {
            xSemaphoreGive(cleanup_mutex);
            Log::logf(CAT_ARB, LOG_WARN, "[ARB] Ticket %u cancelled by caller\n",
                      t->ticket_id);
            if (t->done) vSemaphoreDelete(t->done);
            free(t);
        } else {
            xSemaphoreGive(t->done);  // hand off; caller frees ticket + sem
            xSemaphoreGive(cleanup_mutex);
        }
    }
}


void Arbiter::init(HardwareSerial &serial, int rx_pin, int tx_pin, uint32_t baud) {
    uart = &serial;
    uart->setRxBufferSize(RX_BUF_SIZE);
    uart->begin(baud, SERIAL_8N1, rx_pin, tx_pin);
    current_baud = baud;

    rx_ready = xSemaphoreCreateBinary();
    cleanup_mutex = xSemaphoreCreateMutex();
    qframe_parser_init(&transparent_parser);
    pq_init();

    xTaskCreatePinnedToCore(rx_task, "uart_rx", RX_TASK_STACK, nullptr,
                            RX_TASK_PRIO, &rx_task_handle, 1);
    xTaskCreatePinnedToCore(arbiter_task, "arbiter", ARBITER_TASK_STACK, nullptr,
                            ARBITER_TASK_PRIO, &arbiter_task_handle, 1);
}

bool Arbiter::submit(uart_ticket_t *ticket) {
    if (!ticket->done) {
        ticket->done = xSemaphoreCreateBinary();
    }
    ticket->ticket_id = next_ticket_id++;
    return pq_push(ticket);
}

bool Arbiter::send_cmd(const char *cmd, cmd_source_t src, cmd_priority_t prio,
                       char *resp_buf, uint16_t *resp_len, uint16_t timeout_ms)
{
    if (timeout_ms == 0) timeout_ms = Config::get().uart_cmd_timeout_ms;

    // Reject all commands while in transparent mode
    if (sys_state == SYS_TRANSPARENT) {
        if (resp_len) *resp_len = 0;
        return false;
    }

    // heap-allocate so arbiter can safely outlive the caller on timeout.
    uart_ticket_t *t = (uart_ticket_t*)calloc(1, sizeof(*t));
    if (!t) return false;
    t->source = src;
    t->priority = prio;
    t->timeout_ms = timeout_ms;
    t->done = xSemaphoreCreateBinary();
    if (!t->done) { free(t); return false; }

    int len = qframe_build_cmd(cmd, t->frame, sizeof(t->frame));
    if (len < 0) {
        vSemaphoreDelete(t->done);
        free(t);
        return false;
    }
    t->frame_len = len;
    t->ticket_id = next_ticket_id++;

    if (!pq_push(t)) {
        vSemaphoreDelete(t->done);
        free(t);
        return false;
    }

    BaseType_t got = xSemaphoreTake(t->done, pdMS_TO_TICKS(timeout_ms + 100));
    if (got != pdTRUE) {
        // timed out. Arbiter may still be processing or about to hand off.
        // Under cleanup_mutex: either absorb a late give, or mark cancelled
        // and let the arbiter free the ticket.
        xSemaphoreTake(cleanup_mutex, portMAX_DELAY);
        if (xSemaphoreTake(t->done, 0) == pdTRUE) {
            // Arbiter gave the sem between our timeout and the mutex take.
            // We own the ticket now; fall through to read response + free.
            xSemaphoreGive(cleanup_mutex);
        } else {
            t->cancelled = true;
            xSemaphoreGive(cleanup_mutex);
            // Do NOT touch t after this point. Arbiter will free it.
            if (resp_len) *resp_len = 0;
            return false;
        }
    }

    bool ok = t->success;

    // BDD baud switching (arbiter mode)
    if (ok && strncmp(cmd, "P S #BDD ", 9) == 0) {
        uint32_t new_baud = parse_bdd_baud(t->resp_payload, t->resp_len);
        if (new_baud && new_baud != current_baud) {
            uart->updateBaudRate(new_baud);
            Log::logf(CAT_ARB, LOG_INFO, "[ARB] BDD arbiter: baud %u -> %u\n",
                      current_baud, new_baud);
            current_baud = new_baud;
        }
    }

    if (resp_buf && t->resp_len > 0) {
        uint16_t copy_len = t->resp_len;
        if (resp_len && *resp_len > 0) {
            copy_len = min(copy_len, (uint16_t)(*resp_len - 1));
        }
        memcpy(resp_buf, t->resp_payload, copy_len);
        resp_buf[copy_len] = '\0';
    }
    if (resp_len) *resp_len = t->resp_len;

    vSemaphoreDelete(t->done);
    free(t);
    return ok;
}

bool Arbiter::send_frame(const uint8_t *frame, uint16_t frame_len,
                         cmd_source_t src, cmd_priority_t prio)
{
    if (sys_state == SYS_TRANSPARENT) return false;
    if (frame_len > QFRAME_MAX_RAW) return false;

    // Heap-allocated: arbiter owns the ticket after push and frees it after send.
    // Caller returns immediately — drop the frame if queue is full.
    uart_ticket_t *ticket = (uart_ticket_t*)malloc(sizeof(uart_ticket_t));
    if (!ticket) return false;

    memset(ticket, 0, sizeof(*ticket));
    ticket->source = src;
    ticket->priority = prio;
    ticket->no_ack = true;
    ticket->done = nullptr;  // no semaphore — arbiter frees ticket
    memcpy(ticket->frame, frame, frame_len);
    ticket->frame_len = frame_len;
    ticket->ticket_id = next_ticket_id++;

    if (!pq_push(ticket)) {
        free(ticket);
        return false;
    }
    return true;
}

system_state_t Arbiter::get_state()         { return sys_state; }
void Arbiter::set_state(system_state_t s)   { sys_state = s; }

static volatile int cached_rop = -1;
static volatile int cached_mhr = -1;
int  Arbiter::get_cached_rop()              { return cached_rop; }
int  Arbiter::get_cached_mhr()              { return cached_mhr; }
void Arbiter::set_cached_rop(int v)         { cached_rop = v; }
void Arbiter::set_cached_mhr(int v)         { cached_mhr = v; }

void Arbiter::enter_transparent(Stream *bridge) {
    transparent_bridge = bridge;
    qframe_parser_reset(&transparent_parser);
    transparent_last_activity = millis();
    transparent_active = true;
    sys_state = SYS_TRANSPARENT;
}

uint32_t Arbiter::transparent_activity() {
    return transparent_last_activity;
}

void Arbiter::exit_transparent() {
    transparent_active = false;
    transparent_bridge = nullptr;
    qframe_parser_reset(&rx_parser);
    qframe_parser_reset(&transparent_parser);
    sys_state = SYS_IDLE;
}

void Arbiter::write_raw(const uint8_t *data, size_t len) {
    if (uart && (transparent_active || sys_state == SYS_OTA_AIRSENSE)) {
        uart->write(data, len);
        uart->flush();
        if (transparent_active) transparent_last_activity = millis();
    }
}

void Arbiter::clear_rx_frames() {
    uint8_t cleared;
    portENTER_CRITICAL(&rx_frame_mux);
    cleared = rx_queue_clear_locked();
    portEXIT_CRITICAL(&rx_frame_mux);
    if (rx_ready) {
        xSemaphoreTake(rx_ready, 0);
    }
    if (cleared > 0) {
        Log::logf(CAT_ARB, LOG_DEBUG, "[ARB] RX queue cleared (%u frame%s) t=%lu\n",
                  cleared, cleared == 1 ? "" : "s", millis());
    }
}

bool Arbiter::wait_frame(qframe_t *out, uint16_t timeout_ms) {
    if (rx_queue_pop(out)) {
        return true;
    }
    if (timeout_ms == 0 || !rx_ready) {
        return false;
    }

    uint32_t start = millis();
    while (true) {
        uint32_t elapsed = (uint32_t)(millis() - start);
        if (elapsed >= timeout_ms) {
            break;
        }
        uint32_t remaining = timeout_ms - elapsed;
        if (xSemaphoreTake(rx_ready, pdMS_TO_TICKS(remaining)) != pdTRUE) {
            break;
        }
        if (rx_queue_pop(out)) {
            return true;
        }
        // A stale coalesced wake can remain after immediate queue drains.
        // Consume it and keep waiting for the real timeout window.
    }
    // Covers a frame pushed exactly as the semaphore wait timed out.
    return rx_queue_pop(out);
}

void Arbiter::set_baud(uint32_t baud) {
    if (uart && baud != current_baud) {
        uart->flush();
        uart->updateBaudRate(baud);
        // Flush RX hardware buffer (contains garbage from old baud)
        while (uart->available()) uart->read();
        qframe_parser_reset(&rx_parser);
        Arbiter::clear_rx_frames();
        Log::logf(CAT_ARB, LOG_INFO, "[ARB] set_baud: %u -> %u\n", current_baud, baud);
        current_baud = baud;
    }
}

uint32_t Arbiter::get_baud() {
    return current_baud;
}

uint32_t Arbiter::bdd_key_to_baud(uint16_t key) {
    switch (key) {
        case 0: return 57600;
        case 1: return 115200;
        case 2: return 460800;
        default: return 0;
    }
}

// Parse BDD response payload, return new baud rate or 0 if not BDD.
static uint32_t parse_bdd_baud(const uint8_t *payload, uint16_t len) {
    if (len < 10 || memcmp(payload, "P S #BDD", 8) != 0) return 0;
    const char *val = qframe_response_value((const char *)payload);
    if (!val) return 0;
    return Arbiter::bdd_key_to_baud((uint16_t)strtoul(val, nullptr, 16));
}

uint32_t Arbiter::get_tx_count()       { return stat_tx; }
uint32_t Arbiter::get_rx_count()       { return stat_rx; }
uint32_t Arbiter::get_l_rx_count()     { return stat_l_rx; }
uint32_t Arbiter::get_timeout_count()  { return stat_timeout; }
uint32_t Arbiter::get_error_count()    { return stat_error; }

static uint32_t lcd_clear_at = 0;

void Arbiter::lcd_message(const char *msg, uint32_t timeout_ms) {
    char cmd[32], resp[8];
    uint16_t rlen;
    rlen = sizeof(resp);
    send_cmd("P S #LCA 0000", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, resp, &rlen);
    snprintf(cmd, sizeof(cmd), "P S #LCT %s", msg);
    rlen = sizeof(resp);
    send_cmd(cmd, CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, resp, &rlen);
    rlen = sizeof(resp);
    send_cmd("P S #LCA 0001", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, resp, &rlen);

    lcd_clear_at = timeout_ms ? millis() + timeout_ms : 0;
}

void Arbiter::lcd_clear() {
    char resp[8];
    uint16_t rlen = sizeof(resp);
    send_cmd("P S #LCA 0000", CMD_SRC_INTERNAL, CMD_PRIO_NORMAL, resp, &rlen);
    lcd_clear_at = 0;
}

// Called from arbiter task idle loop, can't use send_cmd
static void lcd_check() {
    if (lcd_clear_at && millis() >= lcd_clear_at) {
        lcd_clear_at = 0;
        uint8_t frame[32];
        int len = qframe_build_cmd("P S #LCA 0000", frame, sizeof(frame));
        if (len > 0)
            Arbiter::send_frame(frame, len, CMD_SRC_INTERNAL, CMD_PRIO_LOW);
    }
}
