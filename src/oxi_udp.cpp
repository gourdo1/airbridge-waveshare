#include "oxi_udp.h"
#include "oxi_arbiter.h"
#include "app_config.h"
#include "debug_log.h"
#include <lwip/sockets.h>
#include <lwip/inet.h>

#define UDP_TASK_STACK  3072
#define UDP_TASK_PRIO   3

// Protocol: [0x55] [0xAB] [flags] [spo2_lo] [spo2_hi] [hr_lo] [hr_hi]
#define UDP_MAGIC_0     0x55
#define UDP_MAGIC_1     0xAB
#define UDP_PACKET_SIZE 7


static void udp_task(void *param) {
    auto &cfg = Config::get();
    uint16_t port = cfg.udp_oxi_port;
    if (port == 0) {
        Log::logf(CAT_OXI, LOG_DEBUG, "UDP disabled (port=0)\n");
        vTaskDelete(NULL);
        return;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || bind(fd, (sockaddr *)&local, sizeof(local)) < 0) {
        if (fd >= 0) close(fd);
        Log::logf(CAT_OXI, LOG_ERROR, "UDP bind failed on port %u\n", port);
        vTaskDelete(NULL);
        return;
    }
    Log::logf(CAT_OXI, LOG_INFO, "UDP listening on port %u\n", port);

    while (true) {
        // One extra byte distinguishes oversized datagrams without retaining them.
        uint8_t buf[UDP_PACKET_SIZE + 1];
        sockaddr_in remote = {};
        socklen_t remote_size = sizeof(remote);
        int len = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                           (sockaddr *)&remote, &remote_size);
        if (len < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        vTaskDelay(1);  // yield after each packet

        if (len != UDP_PACKET_SIZE) {
            Log::logf(CAT_OXI, LOG_DEBUG, "UDP bad length %d (expected %d)\n",
                      len, UDP_PACKET_SIZE);
            continue;
        }

        if (buf[0] != UDP_MAGIC_0 || buf[1] != UDP_MAGIC_1) {
            Log::logf(CAT_OXI, LOG_WARN, "UDP bad magic %02X %02X\n",
                      buf[0], buf[1]);
            continue;
        }

        if (buf[2] & 0xE0) {
            Log::logf(CAT_OXI, LOG_WARN, "UDP bad flags %02X\n", buf[2]);
            continue;
        }

        uint16_t spo2_raw = buf[3] | (buf[4] << 8);
        uint16_t hr_raw = buf[5] | (buf[6] << 8);
        int16_t spo2 = parse_sfloat(spo2_raw);
        int16_t hr = parse_sfloat(hr_raw);

        if (spo2 >= 0 && spo2 <= 100 && hr >= 0 && hr <= 500) {
            if (OxiArbiter::active_source() == OXI_SRC_NONE) {
                char address[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &remote.sin_addr, address, sizeof(address)))
                    OxiArbiter::set_source_id(address);
            }
            Log::logf(CAT_OXI, LOG_DEBUG, "UDP SpO2=%d HR=%d\n", spo2, hr);
            OxiArbiter::feed(OXI_SRC_UDP, spo2, hr, true);
        } else if (spo2 < 0 || hr < 0) {
            Log::logf(CAT_OXI, LOG_DEBUG, "UDP invalid (raw=%04X,%04X)\n", spo2_raw, hr_raw);
            OxiArbiter::feed(OXI_SRC_UDP, -1, -1, false);
        } else {
            Log::logf(CAT_OXI, LOG_WARN, "UDP out of range (spo2=%d hr=%d)\n", spo2, hr);
        }
    }
}

void OxiUdp::init() {
    xTaskCreatePinnedToCore(udp_task, "oxi_udp", UDP_TASK_STACK,
                            nullptr, UDP_TASK_PRIO, nullptr, 0);
}
