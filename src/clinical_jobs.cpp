#include "clinical_jobs.h"
#include "app_config.h"
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <utility>

namespace ClinicalJobs {
namespace {
constexpr uint32_t DEADLINE_MS = 30000;
constexpr uint32_t RETAIN_MS = 30000;
constexpr uint8_t SLOT_COUNT = 4;
enum State : uint8_t { Free, Queued, Running, Complete };
struct Job {
    State state = Free;
    bool write = false;
    bool delivered = false;
    uint32_t id = 0, queued_ms = 0, completed_ms = 0;
    int code = 0;
    String body, result;
};
Job jobs[SLOT_COUNT];
SemaphoreHandle_t mutex = nullptr;
TaskHandle_t task = nullptr;
Handler handler = nullptr;
uint32_t next_id = 1, active_since = 0;

bool reusable(const Job &job) {
    return job.state == Free || (job.state == Complete &&
        uint32_t(millis() - job.completed_ms) >= (job.delivered ? 2000u : RETAIN_MS));
}

void worker(void *) {
    while (true) {
        Job *job = nullptr;
        xSemaphoreTake(mutex, portMAX_DELAY);
        for (Job &candidate : jobs) {
            if (candidate.state == Complete && reusable(candidate))
                candidate = Job{};
            if (candidate.state == Queued &&
                (!job || int32_t(candidate.id - job->id) < 0)) job = &candidate;
        }
        if (job) {
            job->state = Running;
            active_since = job->queued_ms;
        }
        xSemaphoreGive(mutex);
        if (!job) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        String result;
        int code = timeout_ms() ? handler(job->write, job->body, result) : 504;
        if (!timeout_ms()) code = 504;
        if (code == 504)
            result = "{\"error\":\"settings_deadline\",\"partial_write_possible\":true}";
        xSemaphoreTake(mutex, portMAX_DELAY);
        job->body = String();
        job->result = std::move(result);
        job->code = code;
        job->completed_ms = millis();
        job->state = Complete;
        xSemaphoreGive(mutex);
    }
}
}

void init(Handler callback) {
    if (task) return;
    handler = callback;
    mutex = xSemaphoreCreateMutex();
    if (!mutex) return;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        worker, "clinical", 6144, nullptr, 2, &task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS)
        xTaskCreatePinnedToCore(worker, "clinical", 6144, nullptr, 2, &task, 0);
}

bool submit(bool write, const String &body, uint32_t &id) {
    if (!task || body.length() > 2048 || xSemaphoreTake(mutex, 0) != pdTRUE)
        return false;
    Job *available = nullptr;
    bool mutation_pending = false;
    for (Job &job : jobs) {
        if (reusable(job)) available = &job;
        if (job.write && (job.state == Queued || job.state == Running))
            mutation_pending = true;
    }
    if (!write && !mutation_pending) {
        for (Job &job : jobs) {
            if (!job.write && (job.state == Queued || job.state == Running)) {
                id = job.id;
                xSemaphoreGive(mutex);
                return true;
            }
        }
    }
    if (!available || mutation_pending) {
        xSemaphoreGive(mutex);
        return false;
    }
    *available = Job{};
    available->write = write;
    available->body = body;
    available->id = next_id++;
    if (!next_id) next_id = 1;
    available->queued_ms = millis();
    available->state = Queued;
    id = available->id;
    xSemaphoreGive(mutex);
    xTaskNotifyGive(task);
    return true;
}

int poll(uint32_t id, String &result) {
    if (!task || xSemaphoreTake(mutex, 0) != pdTRUE) return 503;
    int code = 410;
    for (Job &job : jobs) {
        if (job.state == Free || job.id != id) continue;
        if (job.state == Complete) {
            if (uint32_t(millis() - job.completed_ms) < RETAIN_MS) {
                result = job.result;
                code = job.code;
                job.delivered = true;
            }
        } else code = 202;
        break;
    }
    xSemaphoreGive(mutex);
    return code;
}

uint16_t timeout_ms() {
    uint16_t limit = Config::get().uart_cmd_timeout_ms;
    if (!task || xTaskGetCurrentTaskHandle() != task) return limit;
    uint32_t elapsed = millis() - active_since;
    return elapsed >= DEADLINE_MS ? 0 : min(uint32_t(limit), DEADLINE_MS - elapsed);
}
}
