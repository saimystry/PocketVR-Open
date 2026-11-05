// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/timing.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace pocketvr {
void ClockSync::reset() {
    *this = ClockSync{};
}
bool ClockSync::observe(int64_t p0, int64_t c1, int64_t c2, int64_t p3) {
    if (p0 < 0 || c1 < 0 || c2 < c1 || p3 < p0)
        return false;
    const auto elapsed = p3 - p0, service = c2 - c1;
    if (elapsed > 2'000'000'000 || service > elapsed)
        return false;
    const auto rtt = elapsed - service;
    Sample s{static_cast<double>(p0) + static_cast<double>(elapsed) * 0.5,
             static_cast<double>(c1) + static_cast<double>(service) * 0.5, rtt};
    samples_[next_] = s;
    next_ = (next_ + 1) % samples_.size();
    count_ = std::min(count_ + 1, samples_.size());
    const auto begin = samples_.begin(), end = begin + static_cast<ptrdiff_t>(count_);
    const auto best = std::min_element(begin, end, [](auto a, auto b) { return a.rtt < b.rtt; });
    best_rtt_ = best->rtt;
    // Select low-RTT samples throughout the window; center the regression to retain QPC precision.
    double sx = 0, sy = 0;
    size_t n = 0;
    const auto threshold = best_rtt_ + std::max<int64_t>(100'000, best_rtt_ / 5);
    for (auto i = begin; i != end; ++i)
        if (i->rtt <= threshold) {
            sx += i->phone - best->phone;
            sy += i->pc - best->pc;
            ++n;
        }
    const double mx = sx / static_cast<double>(n), my = sy / static_cast<double>(n);
    double xx = 0, xy = 0;
    for (auto i = begin; i != end; ++i)
        if (i->rtt <= threshold) {
            const auto x = i->phone - best->phone - mx, y = i->pc - best->pc - my;
            xx += x * x;
            xy += x * y;
        }
    if (n >= 4 && xx > 1e16) {
        const double estimated = xy / xx;
        if (estimated > 0.999 && estimated < 1.001)
            slope_ = estimated;
    }
    anchor_phone_ = best->phone;
    anchor_pc_ = best->pc;
    return true;
}
static std::optional<int64_t> checked_time(double value) {
    if (!std::isfinite(value) || value < 0 ||
        value >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        return {};
    return static_cast<int64_t>(std::llround(value));
}
std::optional<int64_t> ClockSync::phone_to_pc_ns(int64_t phone) const {
    if (!count_ || phone < 0)
        return {};
    return checked_time(anchor_pc_ + (static_cast<double>(phone) - anchor_phone_) * slope_);
}
std::optional<int64_t> ClockSync::pc_to_phone_ns(int64_t pc) const {
    if (!count_ || pc < 0)
        return {};
    return checked_time(anchor_phone_ + (static_cast<double>(pc) - anchor_pc_) / slope_);
}
double PacingPll::update(double lead, double period) {
    if (!std::isfinite(lead) || !std::isfinite(period) || period <= 0 || period > 100 ||
        std::abs(lead) > 1000)
        return 0;
    const double error = target_ms_ - lead;
    if (!initialized_) {
        filtered_error_ = error;
        initialized_ = true;
    } else
        filtered_error_ += 0.1 * (error - filtered_error_);
    // Moving PC vsync earlier increases arrival lead. Negative phase means earlier.
    const double step = std::clamp(-filtered_error_ * 0.025, -0.1, 0.1);
    phase_ms_ += step;
    return step;
}
double PacingPll::report(double lead, double target, double period) {
    if (!std::isfinite(lead) || !std::isfinite(target) || !std::isfinite(period) || period <= 0 ||
        period > 100 || lead < 0 || lead > period)
        return 0;
    // A frame is equally early for one vsync and late for the next: take the
    // shorter way round.
    auto error = lead - target;
    error -= period * std::round(error / period);
    drift_ms_ = std::clamp(drift_ms_ + 0.2 * error, -4.0, 4.0);
    const double step = std::clamp(0.5 * error + drift_ms_, -5.0, 5.0);
    phase_ms_ += step;
    return step;
}
} // namespace pocketvr
