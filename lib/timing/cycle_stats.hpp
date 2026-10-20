#pragma once
// Scan-cycle timing characterization over a sliding window (soft real-time evidence, not a hard-RT proof).
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace icl {
struct Percentiles {
    std::uint32_t p50 = 0, p95 = 0, p99 = 0, max = 0;
    std::size_t samples = 0;
};

class SlidingPercentiles {
public:
    explicit SlidingPercentiles(std::size_t window) : buf_(window, 0) {}
    void add(std::uint32_t v) {
        buf_[head_] = v;
        head_ = (head_ + 1) % buf_.size();
        if (count_ < buf_.size()) ++count_;
    }
    // Nearest-rank percentiles; O(n log n) on a copy, intended to run ~1x per second, not every scan.
    Percentiles compute() const {
        Percentiles p;
        p.samples = count_;
        if (!count_) return p;
        std::vector<std::uint32_t> v(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(count_));
        std::sort(v.begin(), v.end());
        auto rank = [&](double q) {
            const auto idx = static_cast<std::size_t>(q * static_cast<double>(count_ - 1) + 0.5);
            return v[std::min(idx, count_ - 1)];
        };
        p.p50 = rank(0.50); p.p95 = rank(0.95); p.p99 = rank(0.99); p.max = v.back();
        return p;
    }
    std::size_t size() const { return count_; }
private:
    std::vector<std::uint32_t> buf_;
    std::size_t head_ = 0, count_ = 0;
};

// Best-effort real-time tuning of the calling thread. Returns a short human-readable outcome.
//   Linux:   mlockall + SCHED_FIFO priority 80 (needs CAP_SYS_NICE / rtprio limit; falls back silently).
//   Windows: 1 ms system timer resolution + THREAD_PRIORITY_TIME_CRITICAL.
struct RtOutcome { bool elevated = false; const char* detail = "default scheduler"; };
RtOutcome apply_realtime_tuning();
void release_realtime_tuning();

// Sleep until an absolute steady_clock deadline with the best timer the OS offers:
//   Linux:   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) — absolute deadline, no drift accumulation.
//   Windows: high-resolution waitable timer (Windows 10 1803+), falls back to std::this_thread::sleep_until.
void sleep_until_precise(std::chrono::steady_clock::time_point deadline);
}
