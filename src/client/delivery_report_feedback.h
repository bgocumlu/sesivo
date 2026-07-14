#pragma once

#include "sequence_tracker.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace delivery_report_feedback {

inline constexpr uint64_t MAX_INTERVAL_PACKETS = 1024;
inline constexpr uint64_t MIN_REPORTER_OUTCOMES = 8;

struct State {
    uint32_t report_epoch = 0;
    uint32_t sequence = 0;
    uint64_t total_delivered = 0;
    uint64_t total_lost = 0;
    bool initialized = false;
};

struct Delta {
    enum class Status {
        Applied,
        Rebaseline,
        Stale,
        Implausible,
    } status = Status::Stale;
    uint64_t delivered = 0;
    uint64_t lost = 0;
};

struct LifetimeCounter {
    uint64_t total = 0;
    uint64_t current_observed = 0;

    void observe(uint64_t current) {
        total += current >= current_observed
                     ? current - current_observed
                     : current;
        current_observed = current;
    }

    void end_media_lifetime() {
        current_observed = 0;
    }
};

struct ResolvedOutcomes {
    uint64_t delivered = 0;
    uint64_t lost = 0;
};

inline bool claimed_outcomes_plausible(uint64_t already_claimed,
                                       uint64_t newly_claimed,
                                       uint64_t generated) {
    return already_claimed <= generated &&
           newly_claimed <= generated - already_claimed;
}

inline std::optional<double> robust_loss_rate(
    const std::vector<ResolvedOutcomes>& reporters) {
    std::vector<double> rates;
    rates.reserve(reporters.size());
    for (const auto& reporter: reporters) {
        const uint64_t resolved = reporter.delivered + reporter.lost;
        if (resolved >= MIN_REPORTER_OUTCOMES) {
            rates.push_back(static_cast<double>(reporter.lost) /
                            static_cast<double>(resolved));
        }
    }
    if (rates.empty()) {
        return std::nullopt;
    }
    std::sort(rates.begin(), rates.end());
    return rates[(rates.size() - 1) / 2];
}

inline Delta observe(State& state, uint32_t report_epoch, uint32_t sequence,
                     uint64_t total_delivered, uint64_t total_lost) {
    if (report_epoch == 0 || sequence == 0) {
        return {Delta::Status::Implausible};
    }
    if (!state.initialized) {
        state = State{report_epoch, sequence, total_delivered, total_lost, true};
        return {Delta::Status::Rebaseline};
    }
    if (!sequence_number_after(sequence, state.sequence)) {
        return {};
    }
    const bool new_epoch = state.report_epoch != report_epoch;
    if (new_epoch &&
        !sequence_number_after(report_epoch, state.report_epoch)) {
        return {};
    }
    if (!new_epoch &&
        (total_delivered < state.total_delivered ||
         total_lost < state.total_lost)) {
        return {Delta::Status::Implausible};
    }

    if (state.initialized && new_epoch) {
        state = State{report_epoch, sequence, total_delivered, total_lost, true};
        return {Delta::Status::Rebaseline};
    }

    Delta delta{Delta::Status::Applied};
    delta.delivered = total_delivered - state.total_delivered;
    delta.lost = total_lost - state.total_lost;
    state = State{report_epoch, sequence, total_delivered, total_lost, true};
    if (delta.delivered > MAX_INTERVAL_PACKETS ||
        delta.lost > MAX_INTERVAL_PACKETS - delta.delivered) {
        delta.status = Delta::Status::Rebaseline;
    }
    return delta;
}

}  // namespace delivery_report_feedback
