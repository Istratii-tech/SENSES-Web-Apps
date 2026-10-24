// Minimal stand-in for chromium base/time/time.h. Time is kept in microseconds
// since the Unix epoch (chromium used the Windows epoch internally, but nothing
// in posix_translation depends on that). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_TIME_TIME_H_
#define ARC_WASM_SHIM_BASE_TIME_TIME_H_

#include <stdint.h>
#include <sys/time.h>
#include <time.h>

namespace base {

class TimeDelta {
 public:
  TimeDelta() : delta_(0) {}

  static TimeDelta FromDays(int64_t days) {
    return TimeDelta(days * 86400LL * 1000000LL);
  }
  static TimeDelta FromHours(int64_t hours) {
    return TimeDelta(hours * 3600LL * 1000000LL);
  }
  static TimeDelta FromMinutes(int64_t minutes) {
    return TimeDelta(minutes * 60LL * 1000000LL);
  }
  static TimeDelta FromSeconds(int64_t secs) {
    return TimeDelta(secs * 1000000LL);
  }
  static TimeDelta FromMilliseconds(int64_t ms) { return TimeDelta(ms * 1000LL); }
  static TimeDelta FromMicroseconds(int64_t us) { return TimeDelta(us); }
  static TimeDelta FromSecondsD(double secs) {
    return TimeDelta(static_cast<int64_t>(secs * 1000000.0));
  }
  static TimeDelta FromInternalValue(int64_t delta) { return TimeDelta(delta); }
  static TimeDelta Max() { return TimeDelta(INT64_MAX); }

  int64_t ToInternalValue() const { return delta_; }
  bool is_max() const { return delta_ == INT64_MAX; }

  timespec ToTimeSpec() const {
    timespec ts;
    ts.tv_sec = delta_ / 1000000LL;
    ts.tv_nsec = (delta_ % 1000000LL) * 1000LL;
    return ts;
  }

  int InDays() const { return static_cast<int>(delta_ / (86400LL * 1000000LL)); }
  int InHours() const { return static_cast<int>(delta_ / (3600LL * 1000000LL)); }
  int InMinutes() const { return static_cast<int>(delta_ / (60LL * 1000000LL)); }
  double InSecondsF() const { return static_cast<double>(delta_) / 1000000.0; }
  int64_t InSeconds() const { return delta_ / 1000000LL; }
  double InMillisecondsF() const { return static_cast<double>(delta_) / 1000.0; }
  int64_t InMilliseconds() const { return delta_ / 1000LL; }
  int64_t InMillisecondsRoundedUp() const { return (delta_ + 999LL) / 1000LL; }
  int64_t InMicroseconds() const { return delta_; }

  TimeDelta operator+(TimeDelta other) const { return TimeDelta(delta_ + other.delta_); }
  TimeDelta operator-(TimeDelta other) const { return TimeDelta(delta_ - other.delta_); }
  TimeDelta& operator+=(TimeDelta other) { delta_ += other.delta_; return *this; }
  TimeDelta& operator-=(TimeDelta other) { delta_ -= other.delta_; return *this; }
  TimeDelta operator-() const { return TimeDelta(-delta_); }
  TimeDelta operator*(int64_t a) const { return TimeDelta(delta_ * a); }
  TimeDelta operator/(int64_t a) const { return TimeDelta(delta_ / a); }
  int64_t operator/(TimeDelta a) const { return delta_ / a.delta_; }

  bool operator==(TimeDelta other) const { return delta_ == other.delta_; }
  bool operator!=(TimeDelta other) const { return delta_ != other.delta_; }
  bool operator<(TimeDelta other) const { return delta_ < other.delta_; }
  bool operator<=(TimeDelta other) const { return delta_ <= other.delta_; }
  bool operator>(TimeDelta other) const { return delta_ > other.delta_; }
  bool operator>=(TimeDelta other) const { return delta_ >= other.delta_; }

 private:
  friend class Time;
  friend class TimeTicks;
  explicit TimeDelta(int64_t delta_us) : delta_(delta_us) {}
  int64_t delta_;
};

class Time {
 public:
  static const int64_t kMillisecondsPerSecond = 1000;
  static const int64_t kMicrosecondsPerMillisecond = 1000;
  static const int64_t kMicrosecondsPerSecond = 1000000;
  static const int64_t kNanosecondsPerMicrosecond = 1000;
  static const int64_t kNanosecondsPerSecond = 1000000000;

  Time() : us_(0) {}

  static Time Now() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return Time(ts.tv_sec * 1000000LL + ts.tv_nsec / 1000);
  }
  static Time FromTimeT(time_t tt) { return Time(tt * 1000000LL); }
  static Time FromTimeVal(struct timeval t) {
    return Time(t.tv_sec * 1000000LL + t.tv_usec);
  }
  static Time FromTimeSpec(const timespec& ts) {
    return Time(ts.tv_sec * 1000000LL + ts.tv_nsec / 1000);
  }
  static Time FromDoubleT(double dt) {
    return Time(static_cast<int64_t>(dt * 1000000.0));
  }
  static Time FromInternalValue(int64_t us) { return Time(us); }

  bool is_null() const { return us_ == 0; }
  int64_t ToInternalValue() const { return us_; }
  time_t ToTimeT() const { return static_cast<time_t>(us_ / 1000000LL); }
  double ToDoubleT() const { return static_cast<double>(us_) / 1000000.0; }
  struct timeval ToTimeVal() const {
    struct timeval tv;
    tv.tv_sec = us_ / 1000000LL;
    tv.tv_usec = us_ % 1000000LL;
    return tv;
  }

  TimeDelta operator-(Time other) const { return TimeDelta(us_ - other.us_); }
  Time operator+(TimeDelta delta) const { return Time(us_ + delta.delta_); }
  Time operator-(TimeDelta delta) const { return Time(us_ - delta.delta_); }
  Time& operator+=(TimeDelta delta) { us_ += delta.delta_; return *this; }
  Time& operator-=(TimeDelta delta) { us_ -= delta.delta_; return *this; }

  bool operator==(Time other) const { return us_ == other.us_; }
  bool operator!=(Time other) const { return us_ != other.us_; }
  bool operator<(Time other) const { return us_ < other.us_; }
  bool operator<=(Time other) const { return us_ <= other.us_; }
  bool operator>(Time other) const { return us_ > other.us_; }
  bool operator>=(Time other) const { return us_ >= other.us_; }

 private:
  explicit Time(int64_t us) : us_(us) {}
  int64_t us_;
};

class TimeTicks {
 public:
  TimeTicks() : ticks_(0) {}

  static TimeTicks Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return TimeTicks(ts.tv_sec * 1000000LL + ts.tv_nsec / 1000);
  }
  static TimeTicks FromInternalValue(int64_t ticks) { return TimeTicks(ticks); }

  bool is_null() const { return ticks_ == 0; }
  int64_t ToInternalValue() const { return ticks_; }

  TimeDelta operator-(TimeTicks other) const {
    return TimeDelta(ticks_ - other.ticks_);
  }
  TimeTicks operator+(TimeDelta delta) const {
    return TimeTicks(ticks_ + delta.delta_);
  }
  TimeTicks operator-(TimeDelta delta) const {
    return TimeTicks(ticks_ - delta.delta_);
  }
  TimeTicks& operator+=(TimeDelta delta) { ticks_ += delta.delta_; return *this; }
  TimeTicks& operator-=(TimeDelta delta) { ticks_ -= delta.delta_; return *this; }

  bool operator==(TimeTicks other) const { return ticks_ == other.ticks_; }
  bool operator!=(TimeTicks other) const { return ticks_ != other.ticks_; }
  bool operator<(TimeTicks other) const { return ticks_ < other.ticks_; }
  bool operator<=(TimeTicks other) const { return ticks_ <= other.ticks_; }
  bool operator>(TimeTicks other) const { return ticks_ > other.ticks_; }
  bool operator>=(TimeTicks other) const { return ticks_ >= other.ticks_; }

 private:
  explicit TimeTicks(int64_t ticks) : ticks_(ticks) {}
  int64_t ticks_;
};

inline TimeDelta operator*(int64_t a, TimeDelta td) { return td * a; }

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_TIME_TIME_H_
