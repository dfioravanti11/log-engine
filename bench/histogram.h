#pragma once

#include <algorithm>
#include <cstdio>
#include <vector>

#include "base/types.h"

namespace bench {

// Latency percentiles, kept the boring way: store every sample and sort.
//
// Not a sketch, not HdrHistogram, not log-linear buckets. A benchmark run here is at most
// a few million samples — that is tens of megabytes and a sort at the end, once, off the
// hot path. A bucketed histogram would trade exactness for memory this program does not
// need to save, and every approximate percentile is one more thing a reader has to take
// on trust when the numbers are the whole point of the exercise.
//
// The one thing that *is* subtle here is which timestamp goes in. See `Openloop` below.
class Histogram {
 public:
  void add(base::Nanos sample) { samples_.push_back(sample); }
  void reserve(std::size_t n) { samples_.reserve(n); }

  [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }
  [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

  // Percentile in [0, 100]. Sorts on first use.
  [[nodiscard]] base::Nanos percentile(double p) {
    if (samples_.empty()) return 0;
    if (!sorted_) {
      std::sort(samples_.begin(), samples_.end());
      sorted_ = true;
    }
    const double rank = (p / 100.0) * static_cast<double>(samples_.size() - 1);
    const std::size_t index = static_cast<std::size_t>(rank + 0.5);
    return samples_[std::min(index, samples_.size() - 1)];
  }

  [[nodiscard]] base::Nanos max() {
    return samples_.empty() ? 0 : percentile(100.0);
  }

  void print_line(const char* label, double offered_load) {
    // The offered load travels with the percentiles, always. A p99 without the load it
    // was measured at is not a weak number, it is not a number (§19).
    std::printf("%-22s n=%-9zu p50=%8.3f ms  p99=%8.3f ms  p99.9=%8.3f ms  max=%8.3f ms"
                "  @ %.0f/s offered\n",
                label, count(), base::to_millis_f(percentile(50)),
                base::to_millis_f(percentile(99)), base::to_millis_f(percentile(99.9)),
                base::to_millis_f(max()), offered_load);
  }

 private:
  std::vector<base::Nanos> samples_;
  bool sorted_ = false;
};

}  // namespace bench
