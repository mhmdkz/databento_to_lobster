//
// Created by M on 9/23/2025.
//

#ifndef MESSAGE_HPP
#define MESSAGE_HPP

#endif //MESSAGE_HPP

#pragma once

#include "lob_types.hpp"
#include <fstream>
#include <vector>
#include <unordered_map>
#include <cstdint>

// forward declaration
class BookWriter;

class MessageWriter {
public:
    MessageWriter(std::ofstream& msg_out, BookWriter& book_writer, int depth);

    // Process a group of events that share the same sequence number
    // (i.e. all sub-records normalized from one logical TotalView message,
    // such as Trade+Fill+Cancel for an Order Executed event).
    void ProcessGroup(const std::vector<lob::Event>& group);

    void PrintSummary() const;

private:
    // snapshot_ev is the book snapshot to print alongside this message row.
    // Normally this is ev itself (its own post-state). The one deliberate
    // exception: for matched Trade+Fill+Cancel / Fill+Cancel pairs, callers
    // pass the paired Cancel's snapshot instead, since the actual size
    // reduction for a visible execution only takes effect once the Cancel
    // sub-record is applied to the book (Trade/Fill are no-ops on the book).
    void EmitMessageAndBook(const lob::Event& ev,
        const lob::Event& snapshot_ev,
        int lobster_type);
    void ApplyAdd(const lob::Event& ev);
    void ApplyFill(const lob::Event& ev);
    void ApplyCancel(const lob::Event& ev);

    // depth / price-band gate based on the snapshot being printed for this row
    [[nodiscard]] bool IsWithinVisibleBand(const lob::Event& ev,
        const lob::Event& snapshot_ev) const;

    std::ofstream& msg_out_;
    BookWriter& book_writer_;
    int depth_;
    // order pool: order_id -> OrderState (keeps status instead of erasing immediately)
    std::unordered_map<lob::u64, lob::OrderState> order_state_;
    std::vector<uint64_t> counts_;

    // Counters & metrics
    uint64_t cleanup_cancels_skipped_{ 0 };          // skipped T+F+C cleanup cancels (partial-exec)
    uint64_t cleanup_fulfills_marked_deleted_{ 0 };  // when Fill consumed all & we marked Deleted (skipped emitting Delete)
};