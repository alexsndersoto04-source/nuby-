#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <iomanip>
#include <sstream>

namespace nuby::core {

struct ProfileEvent {
    std::string name;
    double duration_us{0.0};
    double duration_ms{0.0};
    std::string details;
};

class Profiler {
private:
    std::chrono::high_resolution_clock::time_point start_time_;
    std::vector<ProfileEvent> events_;
    std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> active_spans_;

public:
    Profiler() {
        start_time_ = std::chrono::high_resolution_clock::now();
    }

    void reset() {
        events_.clear();
        active_spans_.clear();
        start_time_ = std::chrono::high_resolution_clock::now();
    }

    void start_span(const std::string& name) {
        active_spans_[name] = std::chrono::high_resolution_clock::now();
    }

    double end_span(const std::string& name, const std::string& details = "") {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto it = active_spans_.find(name);
        if (it == active_spans_.end()) return 0.0;

        double us = std::chrono::duration<double, std::micro>(end_time - it->second).count();
        double ms = us / 1000.0;
        active_spans_.erase(it);

        events_.push_back({name, us, ms, details});
        return us;
    }

    void record_event(const std::string& name, double us, const std::string& details = "") {
        events_.push_back({name, us, us / 1000.0, details});
    }

    const std::vector<ProfileEvent>& get_events() const { return events_; }

    double total_duration_us() const {
        double total = 0.0;
        for (const auto& ev : events_) {
            total += ev.duration_us;
        }
        return total;
    }

    std::string to_json() const {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"total_us\": " << total_duration_us() << ",\n";
        ss << "  \"total_ms\": " << (total_duration_us() / 1000.0) << ",\n";
        ss << "  \"events\": [\n";
        for (size_t i = 0; i < events_.size(); ++i) {
            const auto& e = events_[i];
            ss << "    {\n";
            ss << "      \"name\": \"" << e.name << "\",\n";
            ss << "      \"duration_us\": " << e.duration_us << ",\n";
            ss << "      \"duration_ms\": " << e.duration_ms << ",\n";
            ss << "      \"details\": \"" << e.details << "\"\n";
            ss << "    }" << (i + 1 < events_.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}";
        return ss.str();
    }
};

class ScopedTimer {
private:
    Profiler& profiler_;
    std::string name_;
    std::string details_;

public:
    ScopedTimer(Profiler& p, const std::string& name, const std::string& details = "")
        : profiler_(p), name_(name), details_(details) {
        profiler_.start_span(name_);
    }

    ~ScopedTimer() {
        profiler_.end_span(name_, details_);
    }
};

} // namespace nuby::core
