#include "clinical_jobs.h"
#include "app_config.h"
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <utility>
#include <new>
#include <atomic>

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
    uint16_t readers = 0;
    uint32_t id = 0, queued_ms = 0, completed_ms = 0;
    int code = 0;
    String body, result;
    ClinicalSettings::Snapshot snapshot;
};
Job jobs[SLOT_COUNT];
SemaphoreHandle_t mutex = nullptr;
std::atomic<TaskHandle_t> task{nullptr};
Handler handler = nullptr;
uint32_t next_id = 1, active_since = 0;

bool reusable(const Job &job) {
    return job.state == Free || (job.state == Complete && !job.readers &&
        uint32_t(millis() - job.completed_ms) >= (job.delivered ? 2000u : RETAIN_MS));
}

void reset_job(Job &job) {
    // Arduino String move-assignment from an empty value retains capacity.
    job.~Job();
    new (&job) Job;
}

void worker(void *) {
    while (true) {
        CustomSettings::reclaim();
        Job *job = nullptr;
        String body;
        xSemaphoreTake(mutex, portMAX_DELAY);
        for (Job &candidate : jobs) {
            if (candidate.state == Complete && reusable(candidate))
                reset_job(candidate);
            if (candidate.state == Queued &&
                (!job || int32_t(candidate.id - job->id) < 0)) job = &candidate;
        }
        if (!job) {
            task = nullptr;
            xSemaphoreGive(mutex);
            break;
        }
        job->state = Running;
        active_since = job->queued_ms;
        body = std::move(job->body);
        xSemaphoreGive(mutex);
        String result;
        int code = 504;
        if (timeout_ms()) {
            if (job->write) code = handler(body, result);
            else {
                code = ClinicalSettings::collect(job->snapshot);
                if (code != 200) result = job->snapshot.error();
            }
        }
        if (!timeout_ms()) code = 504;
        if (code == 504) {
            job->snapshot.reset();
            result = "{\"error\":\"settings_deadline\",\"partial_write_possible\":true}";
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        job->result = std::move(result);
        job->code = code;
        job->completed_ms = millis();
        job->state = Complete;
        xSemaphoreGive(mutex);
    }
    CustomSettings::reclaim();
    vTaskDeleteWithCaps(nullptr);
}

bool start_worker() {
    TaskHandle_t created_task = nullptr;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        worker, "clinical", 6144, nullptr, 2, &created_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS)
        created = xTaskCreatePinnedToCoreWithCaps(
            worker, "clinical", 6144, nullptr, 2, &created_task, 0,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created == pdPASS) task = created_task;
    return created == pdPASS;
}
}

void init(Handler callback) {
    if (mutex) return;
    handler = callback;
    mutex = xSemaphoreCreateMutex();
}

void tick() {
    if (!mutex || xSemaphoreTake(mutex, 0) != pdTRUE) return;
    for (Job &job : jobs)
        if (job.state == Complete && reusable(job)) reset_job(job);
    xSemaphoreGive(mutex);
    CustomSettings::reclaim();
}

Result::~Result() { reset(); }

void Result::reset() {
    if (!id_) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (Job &job : jobs) {
        if (job.id != id_) continue;
        if (job.readers) job.readers--;
        if (reusable(job)) reset_job(job);
        break;
    }
    xSemaphoreGive(mutex);
    id_ = 0;
    data_ = nullptr;
    snapshot_ = nullptr;
    length_ = 0;
}

size_t Result::read(ClinicalSettings::Cursor &cursor, size_t offset,
                    char *out, size_t capacity) const {
    if (snapshot_) return cursor.read(*snapshot_, out, capacity);
    if (!data_ || offset >= length_) return 0;
    size_t count = min(capacity, length_ - offset);
    memcpy(out, data_ + offset, count);
    return count;
}

bool submit(bool write, const String &body, uint32_t &id) {
    if (!mutex || body.length() > 2048 || xSemaphoreTake(mutex, 0) != pdTRUE)
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
    reset_job(*available);
    available->write = write;
    available->body = body;
    if (available->body.length() != body.length()) {
        reset_job(*available);
        xSemaphoreGive(mutex);
        return false;
    }
    available->id = next_id++;
    if (!next_id) next_id = 1;
    available->queued_ms = millis();
    available->state = Queued;
    if (!task && !start_worker()) {
        reset_job(*available);
        xSemaphoreGive(mutex);
        return false;
    }
    id = available->id;
    xSemaphoreGive(mutex);
    return true;
}

int poll(uint32_t id, Result &result) {
    result.reset();
    if (!mutex || xSemaphoreTake(mutex, 0) != pdTRUE) return 503;
    int code = 410;
    for (Job &job : jobs) {
        if (job.state == Free || job.id != id) continue;
        if (job.state == Complete) {
            if (uint32_t(millis() - job.completed_ms) < RETAIN_MS) {
                size_t length = job.snapshot.length() ? job.snapshot.length() : job.result.length();
                if (!length || job.readers == UINT16_MAX) {
                    code = 503;
                    break;
                }
                job.readers++;
                result.id_ = job.id;
                result.snapshot_ = job.snapshot.length() ? &job.snapshot : nullptr;
                result.data_ = result.snapshot_ ? nullptr : job.result.c_str();
                result.length_ = length;
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
    if (xTaskGetCurrentTaskHandle() != task.load()) return limit;
    uint32_t elapsed = millis() - active_since;
    return elapsed >= DEADLINE_MS ? 0 : min(uint32_t(limit), DEADLINE_MS - elapsed);
}
}
