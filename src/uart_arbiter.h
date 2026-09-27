#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "qframe.h"

typedef enum {
    CMD_SRC_OXI,
    CMD_SRC_TCP,
    CMD_SRC_INTERNAL,
    CMD_SRC_OTA,
} cmd_source_t;

typedef enum {
    CMD_PRIO_LOW      = 0,   // oximetry feed, status polls
    CMD_PRIO_NORMAL   = 1,   // TCP client commands
    CMD_PRIO_HIGH     = 2,   // safety queries, session detect
    CMD_PRIO_CRITICAL = 3,   // OTA (exclusive)
} cmd_priority_t;

typedef enum {
    SYS_IDLE,
    SYS_THERAPY,
    SYS_BOOTLOADER,
    SYS_OTA_ESP,
    SYS_OTA_AIRSENSE,
    SYS_TRANSPARENT,
    SYS_ERROR,
} system_state_t;

inline const char *system_state_name(system_state_t s) {
    static const char *names[] = {
        "IDLE","THERAPY","BOOTLOADER","OTA_ESP","OTA_AIRSENSE","TRANSPARENT","ERROR"
    };
    return (s < sizeof(names)/sizeof(names[0])) ? names[s] : "?";
}

typedef bool (*uart_frame_sink_t)(const qframe_t *frame, void *context);

typedef struct {
    qframe_type_mask_t accepted_types;
    qframe_type_mask_t terminal_types;
    qframe_type_mask_t success_types;
    uint16_t first_timeout_ms;
    uint16_t interframe_timeout_ms;
    uint32_t overall_timeout_ms;
    bool complete_on_idle;
} uart_response_policy_t;

typedef struct {
    bool success;
    bool timed_out;
    bool protocol_error;
    bool sink_failed;
    uint8_t terminal_type;
    uint16_t frame_count;
} uart_transaction_result_t;

struct uart_transaction_t;
typedef int8_t uart_frame_listener_t;

namespace Arbiter {
    enum class VarResult { Ok, Missing, Failed };

    void init(HardwareSerial &serial, int rx_pin, int tx_pin);

    bool send_cmd(const char *cmd, cmd_source_t src, cmd_priority_t prio,
                  char *resp_buf, uint16_t *resp_len,
                  uint16_t timeout_ms = 0);  // 0 = use cfg.uart_cmd_timeout_ms

    // Async single R/E response. Capacity includes the terminating byte.
    // Finish only after transaction_done(); otherwise cancel_transaction().
    uart_transaction_t *begin_cmd(const char *cmd, cmd_source_t src,
                                  cmd_priority_t prio, uint16_t capacity,
                                  uint16_t timeout_ms = 0);
    bool finish_cmd(uart_transaction_t *transaction, char *out, uint16_t *length);

    // Return only the scalar value; a truncated value is a failed read.
    VarResult read_var(const char *name, cmd_source_t src, cmd_priority_t prio,
                       char *out, uint16_t capacity, uint16_t timeout_ms = 0);
    VarResult read_var_hex(const char *name, cmd_source_t src, cmd_priority_t prio,
                           uint32_t &out, uint16_t timeout_ms = 0);
    bool get_var(const char *name, cmd_source_t src, cmd_priority_t prio,
                 char *out, uint16_t capacity, uint16_t timeout_ms = 0);

    bool send_frame(const uint8_t *frame, uint16_t frame_len,
                    cmd_source_t src, cmd_priority_t prio);

    uart_transaction_t *begin_cmd(const char *cmd,
                                  cmd_source_t src,
                                  cmd_priority_t prio,
                                  const uart_response_policy_t &policy,
                                  uart_frame_sink_t sink = nullptr,
                                  const void *sink_context = nullptr,
                                  size_t context_size = 0);

    uart_transaction_t *begin_frame(const uint8_t *frame,
                                    uint16_t frame_len,
                                    cmd_source_t src,
                                    cmd_priority_t prio,
                                    const uart_response_policy_t &policy,
                                    uart_frame_sink_t sink = nullptr,
                                    const void *sink_context = nullptr,
                                    size_t context_size = 0);

    // The caller owns a returned transaction until exactly one successful
    // finish_transaction() or cancel_transaction() call. Context is a copied
    // POD value owned by the transaction; it must not reference caller storage.
    // Cancellation detaches immediately, including from an active transaction.
    bool transaction_done(const uart_transaction_t *transaction);
    bool transaction_expired(const uart_transaction_t *transaction);
    bool finish_transaction(uart_transaction_t *transaction,
                            uart_transaction_result_t *result,
                            uint32_t wait_ms = 0,
                            void *context_out = nullptr,
                            size_t context_size = 0);
    void cancel_transaction(uart_transaction_t *transaction);

    bool transact_cmd(const char *cmd,
                      cmd_source_t src,
                      cmd_priority_t prio,
                      const uart_response_policy_t &policy,
                      uart_frame_sink_t sink,
                      void *sink_context,
                      uart_transaction_result_t *result,
                      size_t context_size);

    uart_frame_listener_t add_frame_listener(qframe_type_mask_t types,
                                             uart_frame_sink_t sink,
                                             void *context);
    // Waits for callbacks already in flight. Do not call from the listener.
    void remove_frame_listener(uart_frame_listener_t listener);

    system_state_t get_state();
    void set_state(system_state_t state);
    bool wait_idle(uint16_t timeout_ms);

    // Each session starts at 57600; BDD may change baud within the session.
    void enter_transparent(Stream *bridge);
    // Detach the RX sink and wait for its in-flight calls before returning.
    void exit_transparent();

    void write_raw(const uint8_t *data, size_t len);

    void clear_rx_frames();
    bool wait_frame(qframe_t *out, uint16_t timeout_ms);

    void set_baud(uint32_t baud);
    uint32_t get_baud();

    // BDD key (0=57600, 1=115200, 2=460800)
    uint32_t bdd_key_to_baud(uint16_t key);

    uint32_t get_tx_count();
    uint32_t get_rx_count();
    uint32_t get_l_rx_count();
    uint32_t get_timeout_count();
    uint32_t get_error_count();

    // Queue a copied LCD request without waiting for UART; false = not queued.
    bool lcd_message(const char *msg, uint32_t timeout_ms = 0);  // 0 = persistent
    bool lcd_clear();

    uint32_t transparent_activity();
}
