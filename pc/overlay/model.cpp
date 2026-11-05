// SPDX-License-Identifier: Apache-2.0
#include "model.hpp"
#include "diagnostics.hpp"
#include <pocketvr/settings.hpp>
#include <cmath>

namespace pocketvr::dashboard {
namespace {
void push(std::vector<float> &values, double value) {
    if (!std::isfinite(value) || value < 0) { values.clear(); return; }
    if (values.size() == 120) values.erase(values.begin());
    values.push_back(static_cast<float>(value));
}
}
Client::Client(std::wstring pipe) : pipe_(std::move(pipe)) {
    snapshot_.error = "Waiting for PocketVR driver";
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}
Client::~Client() {
    worker_.request_stop(); wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
Snapshot Client::snapshot() const { std::lock_guard lock(mutex_); return snapshot_; }
bool Client::enqueue(Kind kind, Json request) {
    std::lock_guard lock(mutex_);
    if ((kind == Kind::rpc && !snapshot_.online()) || jobs_.size() >= 16) return false;
    snapshot_.result.clear();
    jobs_.push_back({kind, std::move(request), Clock::now()});
    wake_.notify_all(); return true;
}
bool Client::command(Json request) {
    const auto method = request.value("method", std::string{});
    if (method != "settings.set" && method != "recenter" && method != "pairing.forget" &&
        method != "steamvr.restart" && method != "settings.reset")
        return false;
    if (request.dump().size() > 2048) return false;
    return enqueue(Kind::rpc, std::move(request));
}
bool Client::read_logs() { return enqueue(Kind::logs); }
bool Client::export_bundle() { return enqueue(Kind::bundle); }
void Client::run(std::stop_token stop) {
    const auto call = [&](const Json &request) {
        auto reply = win::rpc(request, pipe_, stop);
        if (!reply.value("ok", false)) throw std::runtime_error(reply.value("error", "Driver rejected request"));
        return reply;
    };
    while (!stop.stop_requested()) {
        try {
            std::optional<Job> job;
            {
                std::lock_guard lock(mutex_);
                if (!jobs_.empty()) { job = std::move(jobs_.front()); jobs_.pop_front(); }
            }
            if (job) {
                std::string result, logs;
                try {
                    if (Clock::now() - job->created > std::chrono::seconds(3))
                        throw std::runtime_error("Request expired; refresh and try again");
                    if (job->kind == Kind::rpc) {
                        (void)call(job->request);
                        result = "Driver accepted the request";
                    } else {
                        const auto current = snapshot();
                        const auto frozen = current.online() ? current.data : Json{{"error", current.error.empty() ? "Driver status stale" : current.error}};
                        const auto directory = win::appdata(true) / "logs";
                        if (job->kind == Kind::logs) {
                            const auto files = win::collect_diagnostics(frozen, directory, stop);
                            for (const auto &file : files) {
                                if (!file.name.starts_with("logs/")) continue;
                                logs += "\n--- " + file.name + " ---\n";
                                logs += file.contents.substr(file.contents.size() > 8192 ? file.contents.size() - 8192 : 0);
                                if (logs.size() > 64 * 1024) { logs.resize(64 * 1024); break; }
                            }
                            if (logs.empty()) logs = "No known PocketVR log files are available";
                            result = "Filtered log tails refreshed";
                        } else {
                            const auto destination = win::appdata(true) / "diagnostics";
                            std::filesystem::create_directories(destination);
                            const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            const auto path = destination / ("PocketVR-" + std::to_string(stamp) + ".zip");
                            win::export_diagnostics(path, frozen, directory, stop);
                            result = "Diagnostics saved in the local PocketVR diagnostics folder. Review before sharing.";
                        }
                    }
                } catch (const std::exception &error) {
                    // Never retry a mutation: a lost reply can follow an applied command.
                    result = std::string("Request failed or outcome uncertain: ") + error.what();
                }
                std::lock_guard lock(mutex_);
                snapshot_.result = std::move(result);
                if (!logs.empty()) snapshot_.logs = std::move(logs);
            }
            Json next;
            next["status"] = call({{"method", "status"}});
            next["network"] = call({{"method", "network.status"}});
            next["settings"] = call({{"method", "settings.get"}});
            next["peers"] = call({{"method", "pairing.list"}}).at("peers");
            // Fail closed on incompatible settings, before enabling mutation widgets.
            (void)Settings(next.at("settings").at("values"));
            if (!next.at("status").is_object() || !next.at("network").is_object() ||
                !next.at("peers").is_array())
                throw std::runtime_error("Incompatible dashboard response");
            std::lock_guard lock(mutex_);
            snapshot_.data = std::move(next); snapshot_.received = Clock::now();
            snapshot_.error.clear(); ++snapshot_.revision;
        } catch (const std::exception &error) {
            std::lock_guard lock(mutex_);
            snapshot_.error = error.what(); snapshot_.data = Json::object();
            jobs_.clear(); ++snapshot_.revision;
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, stop, std::chrono::milliseconds(500), [this] { return !jobs_.empty(); });
    }
}
void History::update(const Snapshot &snapshot) {
    if (!snapshot.online()) {
        bitrate.clear(); decode.clear(); fps.clear(); drops.clear();
        previous = {}; revision = snapshot.revision; return;
    }
    if (revision == snapshot.revision) return;
    revision = snapshot.revision;
    const auto &network = snapshot.data.at("network");
    const auto *session = network.contains("session") && network.at("session").is_object() ? &network.at("session") : nullptr;
    if (!session) { previous = {}; bitrate.clear(); decode.clear(); fps.clear(); drops.clear(); return; }
    const auto sent = session->value("sent_bytes", uint64_t{});
    const auto encoded = session->value("encoded_frames", uint64_t{});
    const auto discarded = session->value("dropped_frames", uint64_t{});
    if (previous != Clock::time_point{} &&
        (snapshot.received <= previous || snapshot.received - previous > std::chrono::seconds(3) ||
         sent < bytes || encoded < frames || discarded < dropped)) {
        bitrate.clear(); decode.clear(); fps.clear(); drops.clear(); previous = {};
    }
    if (previous != Clock::time_point{} && snapshot.received > previous && sent >= bytes && encoded >= frames && discarded >= dropped) {
        const double seconds = std::chrono::duration<double>(snapshot.received - previous).count();
        push(bitrate, static_cast<double>(sent - bytes) * 8 / seconds / 1e6);
        const auto total = (encoded - frames) + (discarded - dropped);
        if (total) push(drops, static_cast<double>(discarded - dropped) * 100 / static_cast<double>(total));
    } else { bitrate.clear(); drops.clear(); }
    previous = snapshot.received; bytes = sent; frames = encoded; dropped = discarded;
    const auto phone = session->value("phone_stats", Json::object());
    if (phone.value("available", false) && phone.contains("values")) {
        const auto &v = phone.at("values");
        if (v.value("decode_samples", uint64_t{}) > 0) push(decode, v.value("decode_p95_ms", -1.0));
        else decode.clear();
        push(fps, v.value("video_fps", -1.0));
    } else { decode.clear(); fps.clear(); }
}
} // namespace pocketvr::dashboard
