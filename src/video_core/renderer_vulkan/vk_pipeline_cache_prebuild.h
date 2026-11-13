// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/unique_function.h"
#include "video_core/renderer_vulkan/pipeline_build.h"
#include "video_core/shader_jobs.h"

namespace Vulkan {

/// Shader cache v2: cached pipelines built in the background as Background jobs of one batch. A
/// lookup of one still queued or building raises it to NeedNow and waits for it, so that it is
/// built once.
template <typename Key, typename Pipeline>
class ScheduledPrebuild {
    struct Entry {
        VideoCommon::ShaderJobHandle job;
        std::unique_ptr<Pipeline> pipeline;
    };

public:
    /// Translates a pipeline on a CPU worker, unbuilt (null when it doesn't translate). Its
    /// Build() runs on a driver worker, and it is dropped when it Failed().
    using Translate = Common::UniqueFunction<std::unique_ptr<Pipeline>>;

    ScheduledPrebuild(VideoCommon::ShaderJobScheduler& jobs_, u64 capture_limit_)
        : jobs{jobs_}, batch{jobs_.CreateBatch()}, capture_limit{capture_limit_} {}

    ~ScheduledPrebuild() {
        Stop();
    }

    ScheduledPrebuild(const ScheduledPrebuild&) = delete;
    ScheduledPrebuild& operator=(const ScheduledPrebuild&) = delete;

    /// Submits the build of a pipeline whose translation holds `capture_bytes` until it runs,
    /// unless one of its key is queued or the captures queued would pass the limit
    /// `done` is called once when the job ends without a Take of it waiting (built, failed,
    /// translated to nothing or stopped by a request); a job cancelled before it ran calls nothing.
    bool Add(const Key& key, u64 capture_bytes, u64 cpu_bytes, u64 driver_bytes,
             Translate translate,
             VideoCommon::ShaderJobPriority priority = VideoCommon::ShaderJobPriority::Background,
             Common::UniqueFunction<void> done = {}) {
        std::scoped_lock lock{mutex};
        if (stopped || entries.contains(key) || capture_bytes > capture_limit - captured_bytes) {
            return false;
        }
        captured_bytes += capture_bytes;
        auto entry = std::make_shared<Entry>();
        entry->job = jobs.Submit(
            {
                .priority = priority,
                .cpu_bytes = cpu_bytes,
                .driver_bytes = driver_bytes,
                .cpu = [this, entry, capture_bytes, translate = std::move(translate),
                        done = std::move(done)](VideoCommon::ShaderJobContext& context) mutable
                -> VideoCommon::ShaderDriverStage {
                    const auto finish = [](Common::UniqueFunction<void>& callback) {
                        if (callback) {
                            callback();
                        }
                    };
                    if (context.StopRequested()) {
                        finish(done);
                        return {};
                    }
                    std::unique_ptr<Pipeline> pipeline = translate();
                    translate = {};
                    {
                        std::scoped_lock capture_lock{mutex};
                        captured_bytes -= capture_bytes;
                    }
                    if (!pipeline) {
                        finish(done);
                        return {};
                    }
                    return [this, entry, pipeline = std::move(pipeline),
                            done = std::move(done)]() mutable {
                        pipeline->Build();
                        if (!pipeline->Failed()) {
                            std::scoped_lock entry_lock{mutex};
                            entry->pipeline = std::move(pipeline);
                        }
                        if (done) {
                            done();
                        }
                    };
                },
            },
            &batch);
        entries.emplace(key, std::move(entry));
        return true;
    }

    /// The pipeline of `key`, built here or waited for while it builds; null when there is none
    /// or it didn't build
    std::unique_ptr<Pipeline> Take(const Key& key) {
        std::shared_ptr<Entry> entry;
        {
            std::scoped_lock lock{mutex};
            const auto it = entries.find(key);
            if (it == entries.end()) {
                return nullptr;
            }
            entry = std::move(it->second);
            entries.erase(it);
        }
        jobs.RunOrWait(entry->job);
        std::scoped_lock lock{mutex};
        return std::move(entry->pipeline);
    }

    /// The batch of the builds, to cancel them without waiting
    const VideoCommon::ShaderJobBatch& Batch() const {
        return batch;
    }

    /// Cancels the builds and drops every pipeline once the running ones end
    void Stop() {
        jobs.Cancel(batch);
        batch.Wait();
        std::unordered_map<Key, std::shared_ptr<Entry>> dropped;
        std::scoped_lock lock{mutex};
        stopped = true;
        dropped = std::move(entries);
        entries.clear();
    }

private:
    VideoCommon::ShaderJobScheduler& jobs;
    VideoCommon::ShaderJobBatch batch;
    const u64 capture_limit;
    u64 captured_bytes{};
    bool stopped{};
    std::mutex mutex;
    std::unordered_map<Key, std::shared_ptr<Entry>> entries;
};

} // namespace Vulkan
