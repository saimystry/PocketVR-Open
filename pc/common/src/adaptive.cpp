// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/adaptive.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pocketvr {
StreamLoadGuard::StreamLoadGuard(unsigned configured_fps) : configured_(configured_fps), cap_(configured_fps) {
    if (configured_fps != 30 && configured_fps != 45 && configured_fps != 60)
        throw std::invalid_argument("Guard FPS must be 30, 45 or 60");
}
GuardDecision StreamLoadGuard::update(uint64_t now, bool enabled, GuardThermal thermal,
                                    double encode_p95, bool encode_fresh) {
    GuardDecision result{cap_};
    if (!enabled) {
        cap_ = configured_; overloaded_ = recovering_ = false; result.reason = "guard disabled";
    } else {
        unsigned thermal_limit = configured_;
        if (thermal.fresh && thermal.status >= 4) thermal_limit = 0;
        else if (thermal.fresh && thermal.status == 3) thermal_limit = std::min(configured_, 30U);
        else if (thermal.fresh && thermal.status == 2) thermal_limit = std::min(configured_, 45U);
        // A paused stream cannot resume on stale/missing cooling information.
        if (!thermal.fresh && cap_ == 0) thermal_limit = 0;
        if (!thermal.fresh && thermal.status >= 2) thermal_limit = std::min(thermal_limit, cap_);
        if (thermal_limit < cap_) {
            cap_ = thermal_limit; recovering_ = overloaded_ = false;
            result.reason = cap_ ? "phone thermal status reduces video cadence" : "phone thermal status pauses video";
        } else {
            const bool measurement = encode_fresh && std::isfinite(encode_p95) && encode_p95 >= 0;
            const bool pressure = cap_ && measurement && encode_p95 > 0.9 * 1000.0 / cap_;
            if (pressure) {
                recovering_ = false; result.reason = "encoder processing exceeds cadence budget";
                if (!overloaded_ || now < overload_since_) { overloaded_ = true; overload_since_ = now; }
                if (now - overload_since_ >= 1500) {
                    cap_ = cap_ > 45 ? 45U : std::min(cap_, 30U);
                    overloaded_ = false;
                }
            } else {
                overloaded_ = false;
                const unsigned next = cap_ == 0 ? std::min(configured_, 30U) :
                    cap_ == 30 ? std::min(configured_, 45U) : configured_;
                const bool headroom = cap_ == 0 ? thermal.fresh && thermal.status >= 0 && thermal.status < 4 :
                    measurement && encode_p95 < 0.8 * 1000.0 / next;
                if (cap_ < configured_ && next <= thermal_limit && headroom) {
                    result.reason = "waiting for sustained recovery headroom";
                    if (!recovering_ || now < recovery_since_) { recovering_ = true; recovery_since_ = now; }
                    if (now - recovery_since_ >= 10'000) {
                        cap_ = next; recovering_ = false; result.reason = "recovering video cadence";
                    }
                } else {
                    recovering_ = false;
                    result.reason = thermal_limit < configured_ ? (cap_ ? "holding thermal video limit" : "cooling down; tracking remains active") :
                        cap_ < configured_ ? "holding processing limit without fresh headroom" : "normal";
                }
            }
        }
    }
    result.changed = result.fps != cap_; result.fps = cap_;
    return result;
}
StreamFecController::StreamFecController(unsigned floor, bool enabled)
    : floor_(floor), ratio_(floor), enabled_(enabled) {
    if (floor > 25) throw std::invalid_argument("FEC floor must be 0-25 percent");
}
FecDecision StreamFecController::update(uint64_t now, const FecFeedback *fresh) {
    FecDecision result{ratio_};
    if (!enabled_) { result.reason = FecReason::fixed; return result; }
    if (!fresh || !fresh->clock_ready || !fresh->sequence) {
        baseline_ = clean_ = false; return result;
    }
    if (baseline_ && fresh->sequence <= previous_.sequence) return result;
    const bool reset = !baseline_ || now <= previous_ms_ || now - previous_ms_ > 3000 ||
        fresh->accepted < previous_.accepted || fresh->reassembly_dropped < previous_.reassembly_dropped ||
        fresh->recovered_groups < previous_.recovered_groups;
    if (reset) {
        previous_ = *fresh; previous_ms_ = now; baseline_ = true; clean_ = false; last_change_ = 0;
        result.reason = FecReason::baseline; return result;
    }
    const auto progress = fresh->accepted - previous_.accepted;
    const auto dropped = fresh->reassembly_dropped - previous_.reassembly_dropped;
    const auto recovered = fresh->recovered_groups - previous_.recovered_groups;
    previous_ = *fresh; previous_ms_ = now;
    if (!progress) { clean_ = false; result.reason = FecReason::idle; }
    else if (dropped) {
        clean_ = false; result.reason = FecReason::pressure;
        if (!last_change_ || now - last_change_ >= 2000) {
            ratio_ = std::min(25U, ratio_ + 7); last_change_ = now;
        }
    } else if (recovered) { clean_ = false; result.reason = FecReason::useful; }
    else {
        result.reason = FecReason::steady;
        if (!clean_) { clean_ = true; clean_since_ = now; }
        if (now - clean_since_ >= 10'000 && (!last_change_ || now - last_change_ >= 2000)) {
            ratio_ = std::max(floor_, ratio_ > 7 ? ratio_ - 7 : 0U);
            last_change_ = now; result.reason = FecReason::clean;
        }
    }
    result.changed = ratio_ != result.ratio; result.ratio = ratio_;
    return result;
}
std::string_view StreamFecController::reason(FecReason reason) {
    switch (reason) {
    case FecReason::baseline: return "establishing FEC feedback baseline";
    case FecReason::fixed: return "fixed FEC setting";
    case FecReason::idle: return "no fresh reassembled video progress";
    case FecReason::pressure: return "unrecovered reassembly drops";
    case FecReason::useful: return "holding useful FEC recovery";
    case FecReason::clean: return "clean active stream reduces extra parity";
    case FecReason::steady: return "holding FEC protection";
    default: return "waiting for fresh FEC feedback";
    }
}
StreamBitrateController::StreamBitrateController(uint32_t initial, uint32_t minimum,
                                                 uint32_t maximum, bool enabled)
    : bitrate_(initial), minimum_(minimum), maximum_(maximum), enabled_(enabled) {
    if (minimum < 10'000'000 || maximum > 150'000'000 || minimum > maximum ||
        initial < 10'000'000 || initial > 150'000'000)
        throw std::invalid_argument("Stream bitrate limits");
    // Fixed quality uses the exact selected bitrate, even outside adaptive limits.
    if (enabled_)
        bitrate_ = std::clamp(initial, minimum_, maximum_);
}
BitrateDecision StreamBitrateController::update(uint64_t now, const BitrateFeedback *fresh) {
    BitrateDecision result{bitrate_};
    if (!enabled_) {
        result.reason = BitrateReason::fixed;
        return result;
    }
    if (!fresh || !fresh->clock_ready || !fresh->sequence) {
        baseline_ = clean_ = false;
        return result;
    }
    if (baseline_ && fresh->sequence <= previous_.sequence)
        return result;
    const bool reset = !baseline_ || now <= previous_ms_ || now - previous_ms_ > 3000 ||
                       fresh->accepted < previous_.accepted ||
                       fresh->reassembly_dropped < previous_.reassembly_dropped ||
                       fresh->sink_rejected < previous_.sink_rejected ||
                       fresh->decoded < previous_.decoded ||
                       fresh->decoder_dropped < previous_.decoder_dropped ||
                       fresh->missed_vsync < previous_.missed_vsync;
    if (reset) {
        last_cut_ = 0;
        baseline_ = true;
        clean_ = false;
        previous_ = *fresh;
        previous_ms_ = now;
        result.reason = BitrateReason::baseline;
        return result;
    }
    const auto elapsed = now - previous_ms_;
    const auto accepted = fresh->accepted - previous_.accepted;
    const auto decoded = fresh->decoded - previous_.decoded;
    // Convert separately to double to avoid overflow when adding peer counters.
    const auto failed =
        static_cast<double>(fresh->reassembly_dropped - previous_.reassembly_dropped) +
        static_cast<double>(fresh->sink_rejected - previous_.sink_rejected);
    const auto attempted = static_cast<double>(accepted) + failed;
    // A rare missed vsync or replaced frame is a phone scheduling hiccup, not a sign the
    // link is full. Cutting on each one drives bitrate to its floor, because recovery
    // needs seconds without any. Only a sustained share of frames counts as pressure.
    const auto display_failed =
        static_cast<double>(fresh->decoder_dropped - previous_.decoder_dropped) +
        static_cast<double>(fresh->missed_vsync - previous_.missed_vsync);
    const bool pressure =
        attempted > 0 && (failed / attempted > 0.02 || display_failed / attempted > 0.05);
    previous_ = *fresh;
    previous_ms_ = now;
    if (pressure) {
        clean_ = false;
        result.reason = BitrateReason::pressure;
        if (!last_cut_ || now - last_cut_ >= 500) {
            bitrate_ = std::max(minimum_, static_cast<uint32_t>(bitrate_ * 0.8));
            last_cut_ = now;
        }
    } else if (!accepted || !decoded) {
        clean_ = false;
        result.reason = BitrateReason::idle;
    } else {
        result.reason = BitrateReason::steady;
        if (!clean_) {
            clean_ = true;
            clean_since_ = now;
        }
        if (now - clean_since_ >= 2000) {
            bitrate_ = static_cast<uint32_t>(std::min<double>(
                maximum_,
                bitrate_ *
                    std::pow(1.05, static_cast<double>(std::min<uint64_t>(elapsed, 1000)) / 1000)));
            result.reason = BitrateReason::recovery;
        }
    }
    result.changed = result.bits_per_second != bitrate_;
    result.bits_per_second = bitrate_;
    return result;
}
std::string_view StreamBitrateController::reason(BitrateReason reason) {
    switch (reason) {
    case BitrateReason::waiting:
        return "waiting for fresh phone feedback";
    case BitrateReason::baseline:
        return "establishing feedback baseline";
    case BitrateReason::fixed:
        return "fixed quality";
    case BitrateReason::idle:
        return "no fresh video progress";
    case BitrateReason::pressure:
        return "frame drops or missed display cadence";
    case BitrateReason::recovery:
        return "clean active stream recovery";
    case BitrateReason::steady:
        return "holding bitrate";
    }
    return "unknown";
}
AdaptiveController::AdaptiveController(Quality start, double minimum, double maximum)
    : quality_(start), minimum_(minimum), maximum_(maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < 10 || maximum > 150 ||
        minimum > maximum || !std::isfinite(start.bitrate_mbps) || start.bitrate_mbps < minimum ||
        start.bitrate_mbps > maximum || start.fec_percent > 25 ||
        (start.fps != 30 && start.fps != 45 && start.fps != 60) || !std::isfinite(start.scale) ||
        start.scale < 0.5 || start.scale > 1.5)
        throw std::invalid_argument("Quality settings");
}
QualityDecision AdaptiveController::update(uint64_t now, const QualityMetrics &m) {
    QualityDecision result{quality_};
    if (!std::isfinite(m.loss_percent) || m.loss_percent < 0 || m.loss_percent > 100 ||
        !std::isfinite(m.decode_p95_ms) || !std::isfinite(m.encode_p95_ms) ||
        !std::isfinite(m.gpu_ms) || !std::isfinite(m.lead_ms))
        return result;
    if (!started_) {
        started_ = true;
        last_update_ = now;
        clean_since_ = now;
        last_step_ = now;
        last_late_ = m.display_late;
        return result;
    }
    if (now < last_update_ || now - last_update_ < 250)
        return result;
    const auto elapsed = std::min<uint64_t>(now - last_update_, 1000);
    last_update_ = now;
    const bool late = m.display_late > last_late_;
    last_late_ = m.display_late;
    if (!enabled_)
        return result;
    const bool congestion = m.loss_percent > 2 || late || m.lead_ms < 1;
    const bool overload =
        m.gpu_ms > 13.5 || m.decode_p95_ms > 15 || m.encode_p95_ms > 12 || m.thermal_status >= 2;
    if (overload && !overloaded_) {
        overloaded_ = true;
        overload_since_ = now;
    } else if (!overload)
        overloaded_ = false;
    if (congestion || overload)
        clean_since_ = now;
    if (congestion && now - last_cut_ >= 500) {
        const auto before = quality_.bitrate_mbps;
        quality_.bitrate_mbps = std::max(minimum_, before * 0.8);
        last_cut_ = now;
        if (quality_.bitrate_mbps < before)
            result.reason = QualityReason::congestion;
        else if (m.bursty && quality_.fec_percent < 25) {
            quality_.fec_percent = std::min(25U, quality_.fec_percent + 5);
            result.reason = QualityReason::burst_protection;
        }
    }
    const bool persistent_congestion = congestion && quality_.bitrate_mbps <= minimum_ &&
                                       (!m.bursty || quality_.fec_percent >= 25);
    if ((persistent_congestion || (overloaded_ && now - overload_since_ >= 2000)) &&
        now - last_step_ >= 2000) {
        if (quality_.scale > 0.85)
            quality_.scale = 0.85;
        else if (quality_.scale > 0.7)
            quality_.scale = 0.7;
        else if (quality_.scale > 0.55)
            quality_.scale = 0.55;
        else if (quality_.fps == 60)
            quality_.fps = 45;
        else if (quality_.fps == 45)
            quality_.fps = 30;
        if (quality_.scale != result.value.scale || quality_.fps != result.value.fps) {
            result.reason =
                m.thermal_status >= 2 ? QualityReason::thermal : QualityReason::processing_overload;
            result.request_idr = true;
            last_step_ = now;
        }
    }
    if (!congestion && !overload) {
        const auto old = quality_.bitrate_mbps;
        quality_.bitrate_mbps =
            std::min(maximum_, old * std::pow(1.05, static_cast<double>(elapsed) / 1000));
        if (quality_.bitrate_mbps > old)
            result.reason = QualityReason::clean_increase;
        if (now - clean_since_ >= 10000 && now - last_step_ >= 10000) {
            if (quality_.fps == 30)
                quality_.fps = 45;
            else if (quality_.fps == 45)
                quality_.fps = 60;
            else if (quality_.scale < 0.7)
                quality_.scale = 0.7;
            else if (quality_.scale < 0.85)
                quality_.scale = 0.85;
            else if (quality_.scale < 1)
                quality_.scale = 1;
            else if (quality_.fec_percent > 0)
                quality_.fec_percent = quality_.fec_percent > 5 ? quality_.fec_percent - 5 : 0;
            if (quality_.fps != result.value.fps || quality_.scale != result.value.scale ||
                quality_.fec_percent != result.value.fec_percent) {
                result.reason = QualityReason::recovery;
                result.request_idr =
                    quality_.fps != result.value.fps || quality_.scale != result.value.scale;
                last_step_ = now;
            }
        }
    }
    result.value = quality_;
    return result;
}
std::string_view AdaptiveController::reason(QualityReason r) {
    switch (r) {
    case QualityReason::none:
        return "unchanged";
    case QualityReason::congestion:
        return "packet loss or late display";
    case QualityReason::clean_increase:
        return "clean link";
    case QualityReason::burst_protection:
        return "bursty loss at bitrate floor";
    case QualityReason::thermal:
        return "phone thermal pressure";
    case QualityReason::processing_overload:
        return "sustained processing or bandwidth overload";
    case QualityReason::recovery:
        return "ten seconds of clean operation";
    }
    return "unknown";
}
} // namespace pocketvr
