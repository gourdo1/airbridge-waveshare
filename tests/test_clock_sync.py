"""Exercise the actual clock-sync functions with a stub UART transport."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ClockSyncTest(unittest.TestCase):
    def test_presence_retry_and_rejection(self):
        source = (ROOT / "src/main.cpp").read_text()
        functions = "\n".join(function(source, signature) for signature in (
            "void reset_resmed_time_sync()",
            "bool push_time_to_resmed()",
            "static void sync_resmed_clock()",
        ))
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
static bool airsense_present = false;
static uint32_t airsense_seen_ms = 0, now_ms = 0, clock_sync_attempt_ms = 0;
static std::atomic<bool> clock_sync_pending{true}, clock_sync_attempted{false};
constexpr uint32_t HEALTH_POLL_INTERVAL_MS = 10000;
constexpr int CMD_SRC_INTERNAL = 0, CMD_PRIO_NORMAL = 0;
constexpr int CAT_GENERAL = 0, LOG_WARN = 0, LOG_INFO = 0;
enum system_state_t { SYS_IDLE, SYS_THERAPY, SYS_ERROR, SYS_TRANSPARENT, SYS_OTA_AIRSENSE };
uint32_t millis() { return now_ms; }
namespace Log { void logf(int, int, const char *, ...) {} }
namespace WiFiSetup {
bool synced = true;
bool time_synced() { return synced; }
}
namespace Arbiter {
system_state_t state = SYS_IDLE;
std::vector<std::string> commands;
int failure_at = -1;
bool rejected = false;
system_state_t get_state() { return state; }
bool send_cmd(const char *cmd, int, int, char *resp, uint16_t *len) {
    commands.emplace_back(cmd);
    if (int(commands.size()) == failure_at) {
        if (rejected) std::snprintf(resp, *len, "600E");
        return false;
    }
    std::snprintf(resp, *len, "%s = OK", cmd);
    return true;
}
}
''' + functions + r'''
int main() {
    sync_resmed_clock();
    assert(Arbiter::commands.empty());
    airsense_present = true;
    WiFiSetup::synced = false;
    sync_resmed_clock();
    assert(Arbiter::commands.empty());
    WiFiSetup::synced = true;
    for (auto state : {SYS_ERROR, SYS_TRANSPARENT, SYS_OTA_AIRSENSE}) {
        Arbiter::state = state;
        sync_resmed_clock();
        assert(Arbiter::commands.empty());
    }
    Arbiter::state = SYS_IDLE;
    now_ms = 10001;
    sync_resmed_clock();
    assert(Arbiter::commands.empty());
    airsense_seen_ms = now_ms;
    Arbiter::failure_at = 1;
    sync_resmed_clock();
    assert(Arbiter::commands.size() == 1 && clock_sync_pending);
    now_ms += 29999;
    airsense_seen_ms = now_ms;
    sync_resmed_clock();
    assert(Arbiter::commands.size() == 1);
    now_ms++;
    sync_resmed_clock();
    assert(Arbiter::commands.size() == 3 && !clock_sync_pending);
    sync_resmed_clock();
    assert(Arbiter::commands.size() == 3);
    for (int rejected_command : {1, 2}) {
        reset_resmed_time_sync();
        Arbiter::commands.clear();
        Arbiter::failure_at = rejected_command;
        Arbiter::rejected = true;
        sync_resmed_clock();
        assert(int(Arbiter::commands.size()) == rejected_command);
        assert(!clock_sync_pending);
        now_ms += 30000;
        airsense_seen_ms = now_ms;
        sync_resmed_clock();
        assert(int(Arbiter::commands.size()) == rejected_command);
    }
    reset_resmed_time_sync();
    Arbiter::commands.clear();
    Arbiter::failure_at = 2;
    Arbiter::rejected = false;
    sync_resmed_clock();
    assert(Arbiter::commands.size() == 2 && clock_sync_pending);
    // The successful DAC response must not turn a TIC timeout into rejection.
    now_ms += 30000;
    airsense_seen_ms = now_ms;
    sync_resmed_clock();
    assert(Arbiter::commands.size() == 4 && !clock_sync_pending);
}
'''
        self.compile_and_run(harness)

    def compile_and_run(self, harness):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "clock_sync.cpp"
            binary = Path(directory) / "clock_sync"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
