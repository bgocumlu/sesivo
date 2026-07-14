#include "sequence_tracker.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_reorder_recovers_gap() {
    SequenceArrivalTracker tracker;

    auto delta = tracker.record(1);
    require(delta.gaps_detected == 0 && tracker.unresolved_gaps() == 0,
            "first packet should initialize tracker");

    delta = tracker.record(3);
    require(delta.gaps_detected == 1, "jump should detect one missing packet");
    require(tracker.unresolved_gaps() == 1, "missing packet should be unresolved");

    delta = tracker.record(2);
    require(delta.late_or_duplicate, "late packet should be marked late/reordered");
    require(delta.gaps_recovered == 1, "late packet should recover the missing gap");
    require(tracker.unresolved_gaps() == 0, "recovered gap should not remain unresolved");
}

void test_sequence_number_comparison_handles_wrap() {
    require(sequence_number_before(UINT32_MAX, 0), "max should be before zero");
    require(sequence_number_after(0, UINT32_MAX), "zero should be after max");
    require(sequence_number_before(41, 42), "lower nearby sequence should be before");
    require(sequence_number_after(42, 41), "higher nearby sequence should be after");
    require(!sequence_number_before(42, 42), "same sequence is not before itself");
    require(!sequence_number_after(42, 42), "same sequence is not after itself");
}

void test_real_loss_stays_unresolved() {
    SequenceArrivalTracker tracker;

    tracker.record(10);
    const auto delta = tracker.record(14);
    require(delta.gaps_detected == 3, "jump should detect three missing packets");
    require(tracker.unresolved_gaps() == 3, "unseen missing packets should stay unresolved");
}

void test_duplicate_is_not_recovery() {
    SequenceArrivalTracker tracker;

    tracker.record(20);
    const auto delta = tracker.record(20);
    require(delta.late_or_duplicate, "duplicate should be marked late/duplicate");
    require(delta.gaps_recovered == 0, "duplicate should not recover a gap");
    require(!sequence_arrival_should_enqueue(delta),
            "duplicate with no recovered gap should not be enqueued");
    require(tracker.unresolved_gaps() == 0, "duplicate should not create unresolved gaps");
}

void test_reordered_recovery_should_enqueue() {
    SequenceArrivalTracker tracker;

    tracker.record(30);
    tracker.record(32);
    const auto delta = tracker.record(31);
    require(delta.late_or_duplicate, "gap recovery should be a late arrival");
    require(delta.gaps_recovered == 1, "gap recovery should report recovered gap");
    require(sequence_arrival_should_enqueue(delta),
            "late arrival that recovers a gap should still be enqueued");
}

void test_wraparound_ordering() {
    SequenceArrivalTracker tracker;

    tracker.record(UINT32_MAX - 1);
    auto delta = tracker.record(1);
    require(delta.gaps_detected == 2, "wrap jump should detect max and zero missing");
    require(tracker.unresolved_gaps() == 2, "wrap missing packets should be unresolved");

    delta = tracker.record(UINT32_MAX);
    require(delta.gaps_recovered == 1, "UINT32_MAX should recover one wrap gap");
    delta = tracker.record(0);
    require(delta.gaps_recovered == 1, "zero should recover the other wrap gap");
    require(tracker.unresolved_gaps() == 0, "wrap gaps should all be recovered");
}

void test_large_gap_recovery_beyond_small_packet_window() {
    SequenceArrivalTracker tracker;

    tracker.record(1000);
    auto delta = tracker.record(2000);
    require(delta.gaps_detected == 999, "large jump should detect all missing packets");
    require(tracker.unresolved_gaps() == 999, "large gap should be unresolved");

    delta = tracker.record(1800);
    require(delta.gaps_recovered == 1,
            "late packet deep inside a large gap should recover");
    require(tracker.unresolved_gaps() == 998,
            "large gap recovery should decrement unresolved count");

    delta = tracker.record(1799);
    require(delta.gaps_recovered == 1, "left split range should still recover");
    delta = tracker.record(1801);
    require(delta.gaps_recovered == 1, "right split range should still recover");
    require(tracker.unresolved_gaps() == 996,
            "split range recoveries should keep decrementing unresolved count");
}

void test_range_overflow_expires_oldest_without_suppressing_totals() {
    SequenceArrivalTracker tracker;

    tracker.record(0);
    for (uint32_t sequence = 2; sequence <= 128; sequence += 2) {
        const auto delta = tracker.record(sequence);
        require(delta.gaps_detected == 1, "separated jump should detect one gap");
    }
    require(tracker.unresolved_gaps() == 64,
            "first 64 separated gaps should be tracked");

    auto delta = tracker.record(130);
    require(delta.gaps_detected == 1, "overflow jump should still report detected gap");
    require(tracker.unresolved_gaps() == 64,
            "new gap should replace the oldest tracked gap");
    require(tracker.total_gaps_detected() == 65,
            "cumulative detected gaps must not be capped by tracking capacity");

    delta = tracker.record(129);
    require(delta.late_or_duplicate, "late newest-gap packet should still be late");
    require(delta.gaps_recovered == 1,
            "newest gap must remain recoverable after range eviction");
    require(tracker.unresolved_gaps() == 63,
            "newest recovery should reduce unresolved count");

    for (uint32_t sequence = 1; sequence <= 127; sequence += 2) {
        delta = tracker.record(sequence);
        require(delta.gaps_recovered == (sequence == 1 ? 0U : 1U),
                "only the expired oldest gap should be unrecoverable");
    }
    require(tracker.unresolved_gaps() == 0,
            "all tracked gaps should recover to zero unresolved");
    require(tracker.total_gaps_recovered() == 64,
            "cumulative recoveries should remain independent of range capacity");
}

void test_full_table_middle_split_expires_an_older_range() {
    SequenceArrivalTracker tracker;

    tracker.record(0);
    for (uint32_t sequence = 2; sequence <= 126; sequence += 2) {
        tracker.record(sequence);
    }
    require(tracker.unresolved_gaps() == 63,
            "first 63 separated gaps should be tracked");

    auto delta = tracker.record(137);
    require(delta.gaps_detected == 10, "large gap should be detected");
    require(tracker.unresolved_gaps() == 73,
            "large range should fill the final tracked slot");

    delta = tracker.record(132);
    require(delta.gaps_recovered == 1, "middle packet should recover one gap");
    require(tracker.unresolved_gaps() == 71,
            "middle split should preserve both halves and expire one older range");

    for (uint32_t sequence = 133; sequence <= 136; ++sequence) {
        delta = tracker.record(sequence);
        require(delta.late_or_duplicate, "right split packet should still be late");
        require(delta.gaps_recovered == 1,
                "right split should remain tracked after older-range eviction");
    }
    require(tracker.unresolved_gaps() == 67,
            "right split recoveries should reduce unresolved count");

    for (uint32_t sequence = 1; sequence <= 125; sequence += 2) {
        delta = tracker.record(sequence);
        require(delta.gaps_recovered == (sequence == 1 ? 0U : 1U),
                "only the expired oldest separated range should be lost");
    }
    for (uint32_t sequence = 127; sequence <= 131; ++sequence) {
        tracker.record(sequence);
    }
    require(tracker.unresolved_gaps() == 0,
            "all remaining tracked split ranges should recover");
}

}  // namespace

int main() {
    test_reorder_recovers_gap();
    test_sequence_number_comparison_handles_wrap();
    test_real_loss_stays_unresolved();
    test_duplicate_is_not_recovery();
    test_reordered_recovery_should_enqueue();
    test_wraparound_ordering();
    test_large_gap_recovery_beyond_small_packet_window();
    test_range_overflow_expires_oldest_without_suppressing_totals();
    test_full_table_middle_split_expires_an_older_range();

    std::cout << "sequence tracker self-test passed\n";
    return 0;
}
