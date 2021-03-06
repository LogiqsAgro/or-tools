// Copyright 2010-2014 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef OR_TOOLS_UTIL_TIME_LIMIT_H_
#define OR_TOOLS_UTIL_TIME_LIMIT_H_

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/port.h"
#include "base/timer.h"
#include "base/time_support.h"
#include "util/running_stat.h"

// Enables to change the behavior of the TimeLimit class to use "usertime"
// instead of walltime. This is mainly useful for benchmarks.
DECLARE_bool(time_limit_use_usertime);

namespace operations_research {

// A simple class to enforce both an elapsed time limit and a deterministic time
// limit in the same thread as a program.
// The idea is to call LimitReached() as often as possible, until it returns
// false. The program should then abort as fast as possible.
//
// The deterministic limit is used to ensure reproductibility, as a consequence
// the deterministic time has to be advanced manually using the method
// AdvanceDeterministicTime().
//
// The call itself is as fast as CycleClock::Now() + a few trivial instructions.
//
// The limit is very conservative: it returns true (i.e. the limit is reached)
// when current_time + std::max(T, ε) >= limit_time, where ε is a small constant (see
// TimeLimit::kSafetyBufferSeconds), and T is the maximum measured time interval
// between two consecutive calls to LimitReached() over the last kHistorySize
// calls (so that we only consider "recent" history).
// This is made so that the probability of actually exceeding the time limit is
// small, without aborting too early.
class TimeLimit {
 public:
  static const double kSafetyBufferSeconds;  // See the .cc for the value.
  static const int kHistorySize;

  // Sets both the elapsed and the deterministic time limits.
  // The elapsed time is based on the wall time and the counter starts 'now'.
  // The deterministic time has to be manually advanced using the method
  // AdvanceDeterministicTime().
  // Use an infinite limit value to ignore a limit.
  TimeLimit(
      double limit_in_seconds,
      double deterministic_limit = std::numeric_limits<double>::infinity());

  // Returns true when the deterministic time is over the deterministic limit or
  // if the next time LimitReached() is called is likely to be over the time
  // limit. See toplevel comment.
  // Once it has returned true, it is guaranteed to always return true.
  bool LimitReached();

  // Returns the time left on this limit, or 0 if the limit was reached (it
  // never returns a negative value). Note that it might return a positive
  // value even though LimitReached() would return true; because the latter is
  // conservative (see toplevel comment).
  // If LimitReached() was actually called and did return "true", though, this
  // will always return 0.
  //
  // If the TimeLimit was constructed with "infinity" as the limit, this will
  // always return infinity.
  //
  // Note that this function is not optimized for speed as LimitReached() is.
  double GetTimeLeft() const;

  // Returns the remaining deterministic time before LimitReached() returns
  // true due to the deterministic limit.
  // If the TimeLimit was constructed with "infinity" as the deterministic
  // limit (default value), this will always return infinity.
  double GetDeterministicTimeLeft() const {
    return std::max(0.0, deterministic_limit_ - elapsed_deterministic_time_);
  }

  // Advances the deterministic time. For reproductibility reasons, the
  // deterministic time doesn't advance automatically as the regular elasped
  // time does.
  void AdvanceDeterministicTime(double deterministic_duration) {
    DCHECK_LE(0.0, deterministic_duration);
    elapsed_deterministic_time_ += deterministic_duration;
  }

  // Returns the time elapsed in seconds since the construction of this object.
  double GetElapsedTime() const {
    return 1e-9 * (base::GetCurrentTimeNanos() - start_ns_);
  }

  // Returns the elapsed deterministic time since the construction of this
  // object. That corresponds to the sum of all deterministic durations passed
  // as an argument to AdvanceDeterministicTime() calls.
  double GetElapsedDeterministicTime() const {
    return elapsed_deterministic_time_;
  }

 private:
  const int64 start_ns_;
  int64 last_ns_;
  int64 limit_ns_;  // Not const! See the code of LimitReached().
  const int64 safety_buffer_ns_;
  RunningMax<int64> running_max_;

  // Only used when FLAGS_time_limit_use_usertime is true.
  UserTimer user_timer_;
  double limit_in_seconds_;

  double deterministic_limit_;
  double elapsed_deterministic_time_;

  DISALLOW_COPY_AND_ASSIGN(TimeLimit);
};

// ################## Implementations below #####################

inline TimeLimit::TimeLimit(double limit_in_seconds, double deterministic_limit)
    : start_ns_(base::GetCurrentTimeNanos()),
      last_ns_(start_ns_),
      limit_ns_(limit_in_seconds >= 1e-9 * (kint64max - start_ns_)
                    ? kint64max
                    : static_cast<int64>(limit_in_seconds * 1e9) + start_ns_),
      safety_buffer_ns_(static_cast<int64>(kSafetyBufferSeconds * 1e9)),
      running_max_(kHistorySize),
      deterministic_limit_(deterministic_limit),
      elapsed_deterministic_time_(0.0) {
  if (FLAGS_time_limit_use_usertime) {
    user_timer_.Start();
    limit_in_seconds_ = limit_in_seconds;
  }
}

inline bool TimeLimit::LimitReached() {
  if (GetDeterministicTimeLeft() <= 0.0) {
    return true;
  }

  const int64 current_ns = base::GetCurrentTimeNanos();
  running_max_.Add(std::max(safety_buffer_ns_, current_ns - last_ns_));
  last_ns_ = current_ns;
  if (current_ns + running_max_.GetCurrentMax() >= limit_ns_) {
    if (FLAGS_time_limit_use_usertime) {
      // To avoid making many system calls, we only check the user time when
      // the "absolute" time limit has been reached. Note that the user time
      // should advance more slowly, so this is correct.
      const double time_left_s = limit_in_seconds_ - user_timer_.Get();
      if (time_left_s > kSafetyBufferSeconds) {
        limit_ns_ = static_cast<int64>(time_left_s * 1e9) + last_ns_;
        return false;
      }
    }

    // To ensure that future calls to LimitReached() will return true.
    limit_ns_ = 0;
    return true;
  }
  return false;
}

inline double TimeLimit::GetTimeLeft() const {
  if (limit_ns_ == kint64max) return std::numeric_limits<double>::infinity();
  const int64 delta_ns = limit_ns_ - base::GetCurrentTimeNanos();
  if (delta_ns < 0) return 0.0;
  if (FLAGS_time_limit_use_usertime) {
    return std::max(limit_in_seconds_ - user_timer_.Get(), 0.0);
  } else {
    return delta_ns * 1e-9;
  }
}

}  // namespace operations_research

#endif  // OR_TOOLS_UTIL_TIME_LIMIT_H_
