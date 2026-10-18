#include "message.hpp"
#include "book.hpp"
#include "lob_types.hpp"

#include <fmt/format.h>

#include <algorithm>  // std::min
#include <limits>     // std::numeric_limits
#include <vector>

namespace {

    constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    std::size_t FindNextFillIndex(const std::vector<lob::Event>& group, std::size_t start) {
        for (std::size_t j = start; j < group.size(); ++j) {
            if (group[j].action == lob::Action::Fill) {
                return j;
            }
        }
        return kNpos;
    }

    std::size_t FindMatchingCancelIndex(const std::vector<lob::Event>& group,
        std::size_t start,
        lob::u64 order_id,
        const std::vector<bool>& skipped) {
        for (std::size_t j = start; j < group.size(); ++j) {
            if (skipped[j]) continue;
            if (group[j].action == lob::Action::Cancel && group[j].order_id == order_id) {
                return j;
            }
        }
        return kNpos;
    }

} // namespace

// Constructor
MessageWriter::MessageWriter(std::ofstream& msg_out, BookWriter& book_writer, int depth)
    : msg_out_(msg_out), book_writer_(book_writer), depth_(depth) {
    order_state_.reserve(1 << 20);
    counts_.assign(8, 0);
}

/*
 Depth / price-band gate:
 Use the snapshot we are about to print to build a band from the first `depth_`
 bid and ask levels. Only emit messages whose ev.price lies within that band.
*/
bool MessageWriter::IsWithinVisibleBand(const lob::Event& ev,
    const lob::Event& snapshot_ev) const {
    if (depth_ <= 0) {
        return true;
    }

    const double p = ev.price;
    if (p == 0.0) {
        return true;
    }

    double band_min = std::numeric_limits<double>::infinity();
    double band_max = -std::numeric_limits<double>::infinity();
    bool have_price = false;

    auto accumulate_side = [&](const std::vector<double>& px_vec) {
        const int limit = std::min(depth_, static_cast<int>(px_vec.size()));
        for (int lvl = 0; lvl < limit; ++lvl) {
            const double lv = px_vec[lvl];
            if (lv == 0.0) {
                continue;
            }
            if (!have_price) {
                band_min = band_max = lv;
                have_price = true;
            }
            else {
                if (lv < band_min) band_min = lv;
                if (lv > band_max) band_max = lv;
            }
        }
        };

    accumulate_side(snapshot_ev.bid_px);
    accumulate_side(snapshot_ev.ask_px);

    if (!have_price) {
        return true;
    }

    constexpr double eps = 1e-9;
    if (p < band_min - eps) return false;
    if (p > band_max + eps) return false;
    return true;
}

/*
 Helper: emit message row + corresponding orderbook row (snapshot_ev).
 lobster_type follows LOBSTER mapping: 1=Add,2=PartialCancel,3=Delete,
 4=Execution(visible)/Fill,5=HiddenTrade

 snapshot_ev is the book state to print for this row. Callers pass ev's own
 post-state in almost every case; the two matched-pair branches below pass
 the paired Cancel's post-state instead (see message.hpp for why).
*/
void MessageWriter::EmitMessageAndBook(const lob::Event& ev,
    const lob::Event& snapshot_ev,
    int lobster_type) {
    if (!IsWithinVisibleBand(ev, snapshot_ev)) {
        return;
    }

    double secs = 0.0;
    if (!lob::ParseTsToSeconds(ev.ts_event, secs)) secs = 0.0;

    long long lobster_price = 0;
    if (ev.price != 0.0) lobster_price = lob::PriceToLobsterInt(ev.price);

    int direction = (ev.side == lob::Side::Ask) ? -1 : 1;

    msg_out_ << fmt::format("{:.9f},{},{},{},{},{}\n",
        secs, lobster_type, ev.order_id, ev.size, lobster_price, direction);

    book_writer_.EmitOrderbookRow(snapshot_ev);

    if (lobster_type > 0) {
        if (const auto idx = static_cast<std::size_t>(lobster_type); idx < counts_.size()) {
            ++counts_[idx];
        }
    }
}

/* Add: create or update the order in pool as Active */
void MessageWriter::ApplyAdd(const lob::Event& ev) {
    lob::OrderState st;
    st.remaining = ev.size;
    st.original_size = ev.size;
    st.side = ev.side;
    st.price = ev.price;
    st.status = lob::OrderState::Status::Active;
    st.first_seen_ts = ev.ts_event;
    st.last_seen_ts = ev.ts_event;
    st.total_filled = 0;
    st.total_canceled = 0;
    order_state_[ev.order_id] = st;
}

/*
 Fill: reduce remaining; if fully consumed set status to Deleted.
 Also update tracing counters.
*/
void MessageWriter::ApplyFill(const lob::Event& ev) {
    if (const auto it = order_state_.find(ev.order_id); it != order_state_.end()) {
        if (it->second.remaining <= ev.size) {
            it->second.total_filled += it->second.remaining;
            it->second.remaining = 0;
            it->second.status = lob::OrderState::Status::Deleted;
        }
        else {
            it->second.remaining -= ev.size;
            it->second.total_filled += ev.size;
        }
        it->second.last_seen_ts = ev.ts_event;
    }
    else {
        fmt::print(stderr, "Warning: Fill for unknown order {} at {}\n", ev.order_id, ev.ts_event);
    }
}

/*
 Cancel: update pool and tracing
*/
void MessageWriter::ApplyCancel(const lob::Event& ev) {
    auto it = order_state_.find(ev.order_id);
    if (it == order_state_.end()) {
        fmt::print(stderr, "Warning: Cancel for unknown order {} at {}\n", ev.order_id, ev.ts_event);
        return;
    }

    auto& st = it->second;
    switch (st.status) {
    case lob::OrderState::Status::Active: {
        if (ev.size < st.remaining) {
            st.remaining -= ev.size;
            st.total_canceled += ev.size;
            st.last_seen_ts = ev.ts_event;
        }
        else {
            st.total_canceled += st.remaining;
            st.remaining = 0;
            st.status = lob::OrderState::Status::Deleted;
            st.last_seen_ts = ev.ts_event;
        }
        break;
    }
    case lob::OrderState::Status::Deleted: {
        fmt::print(stderr, "Warning: Cancel for already-deleted order {} at {}\n", ev.order_id, ev.ts_event);
        break;
    }
    default: {
        st.status = lob::OrderState::Status::Deleted;
        st.last_seen_ts = ev.ts_event;
        break;
    }
    }
}

/*
 Process a group of events with identical sequence number (i.e. all
 sub-records normalized from one logical TotalView message).

 Supported visible execution patterns:
  - Trade + Fill + Cancel
  - Fill + Cancel (omitted/non-printable Trade)
  - Fill alone (standalone execution)

 For cleanup cancels, we search later in the same group for the matching
 Cancel by order_id and skip it so it never reaches the standalone Cancel branch.

 Snapshot rule: every emitted row uses ev's own post-state, EXCEPT the two
 matched-pair branches (T+F+C and F+C), which deliberately use the paired
 Cancel's post-state — Trade/Fill are no-ops on the book, so only the Cancel
 sub-record reflects the actual size reduction for a visible execution.
*/
void MessageWriter::ProcessGroup(const std::vector<lob::Event>& group) {
    if (group.empty()) return;

    std::vector<bool> skipped(group.size(), false);

    std::size_t i = 0;
    while (i < group.size()) {
        if (skipped[i]) {
            ++i;
            continue;
        }

        const lob::Event& ev = group[i];

        // Trade + Fill (+ later Cancel cleanup)
        if (ev.action == lob::Action::Trade) {
            if (i + 1 < group.size() && !skipped[i + 1] && group[i + 1].action == lob::Action::Fill) {
                const std::size_t fill_idx = i + 1;
                const lob::Event& fill_ev = group[fill_idx];

                const std::size_t cancel_idx = FindMatchingCancelIndex(group, fill_idx + 1,
                    fill_ev.order_id, skipped);
                if (cancel_idx != kNpos) {
                    const lob::Event& cancel_ev = group[cancel_idx];

                    EmitMessageAndBook(fill_ev, cancel_ev, 4);
                    ApplyFill(fill_ev);

                    auto it = order_state_.find(fill_ev.order_id);
                    if (it != order_state_.end()) {
                        if (it->second.remaining == 0) {
                            ++cleanup_fulfills_marked_deleted_;
                        }
                        else {
                            ++cleanup_cancels_skipped_;
                        }
                    }
                    else {
                        fmt::print(stderr, "Warning: T+F+C cleanup references unknown order {} at {}\n",
                            fill_ev.order_id, fill_ev.ts_event);
                        ++cleanup_cancels_skipped_;
                    }

                    skipped[fill_idx] = true;
                    skipped[cancel_idx] = true;
                    ++i;
                    continue;
                }

                // No matching Cancel found in this group. Per ITCH normalization,
                // Order Executed should always be followed by a Cancel — if we get
                // here, either the Cancel legitimately fell into a different group
                // (should no longer happen now that grouping is keyed by sequence)
                // or it's genuine data loss upstream. Emit best-effort using the
                // Fill's own snapshot, warn loudly, and mark it skipped so the main
                // loop doesn't reprocess it as a standalone Fill.
                fmt::print(stderr, "Warning: Trade+Fill with no matching Cancel found for order {} at {}\n",
                    fill_ev.order_id, fill_ev.ts_event);
                EmitMessageAndBook(fill_ev, fill_ev, 4);
                ApplyFill(fill_ev);
                skipped[fill_idx] = true;

                ++i;
                continue;
            }

            if (ev.side == lob::Side::NoneSide) {
                EmitMessageAndBook(ev, ev, 5);
            }
            else {
                EmitMessageAndBook(ev, ev, 4);
            }
            ++i;
            continue;
        }

        // Fill + Cancel cleanup (omitted Trade case)
        if (ev.action == lob::Action::Fill) {
            const std::size_t cancel_idx = FindMatchingCancelIndex(group, i + 1, ev.order_id, skipped);
            if (cancel_idx != kNpos) {
                const lob::Event& cancel_ev = group[cancel_idx];

                EmitMessageAndBook(ev, cancel_ev, 4);
                ApplyFill(ev);

                auto it = order_state_.find(ev.order_id);
                if (it != order_state_.end()) {
                    if (it->second.remaining == 0) {
                        ++cleanup_fulfills_marked_deleted_;
                    }
                    else {
                        ++cleanup_cancels_skipped_;
                    }
                }
                else {
                    fmt::print(stderr, "Warning: T+F+C cleanup references unknown order {} at {}\n",
                        ev.order_id, ev.ts_event);
                    ++cleanup_cancels_skipped_;
                }

                skipped[cancel_idx] = true;
                ++i;
                continue;
            }

            fmt::print(stderr, "Warning: Fill with no matching Cancel found for order {} at {}\n",
                ev.order_id, ev.ts_event);
            EmitMessageAndBook(ev, ev, 4);
            ApplyFill(ev);
            ++i;
            continue;
        }

        // Add
        if (ev.action == lob::Action::Add) {
            EmitMessageAndBook(ev, ev, 1);
            auto it = order_state_.find(ev.order_id);
            if (it == order_state_.end() || it->second.status == lob::OrderState::Status::Deleted) {
                // fresh order, or a recycled order_id whose previous occupant was
                // fully closed out — either way, a full reset is correct here.
                ApplyAdd(ev);
            }
            else {
                // still-Active order reusing an id that's supposedly open — should
                // not happen per ITCH spec, but warn rather than fail silently.
                fmt::print(stderr, "Warning: Add for already-active order {} at {}\n", ev.order_id, ev.ts_event);
                it->second.remaining += ev.size;
                if (it->second.original_size == 0) it->second.original_size = ev.size;
                it->second.last_seen_ts = ev.ts_event;
            }
            ++i;
            continue;
        }

        // Cancel (standalone cancel)
        if (ev.action == lob::Action::Cancel) {
            auto it = order_state_.find(ev.order_id);
            if (it != order_state_.end()) {
                if (it->second.status == lob::OrderState::Status::Active) {
                    if (ev.size < it->second.remaining) {
                        EmitMessageAndBook(ev, ev, 2);
                        ApplyCancel(ev);
                    }
                    else {
                        EmitMessageAndBook(ev, ev, 3);
                        ApplyCancel(ev);
                    }
                }
                else if (it->second.status == lob::OrderState::Status::Deleted) {
                    fmt::print(stderr, "Warning: Cancel for already-deleted order {} at {}\n", ev.order_id, ev.ts_event);
                }
                else {
                    ApplyCancel(ev);
                }
            }
            else {
                fmt::print(stderr, "Warning: Cancel for unknown order {} at {}\n", ev.order_id, ev.ts_event);
            }
            ++i;
            continue;
        }

        // Modify — not expected for NASDAQ TotalView-ITCH normalization (Order
        // Replace is already split into Cancel+Add upstream). Kept defensively
        // in case this pipeline is ever pointed at a different venue/dataset.
        if (ev.action == lob::Action::Modify) {
            fmt::print(stderr, "Warning: unexpected Modify for order {} at {} (not expected for NASDAQ ITCH)\n",
                ev.order_id, ev.ts_event);
            auto it = order_state_.find(ev.order_id);
            if (it != order_state_.end()) {
                it->second.remaining = ev.size;
                it->second.last_seen_ts = ev.ts_event;
                if (it->second.remaining == 0) it->second.status = lob::OrderState::Status::Deleted;
            }
            else {
                lob::OrderState st;
                st.remaining = ev.size;
                st.original_size = ev.size;
                st.side = ev.side;
                st.price = ev.price;
                st.status = lob::OrderState::Status::Active;
                st.first_seen_ts = ev.ts_event;
                st.last_seen_ts = ev.ts_event;
                order_state_[ev.order_id] = st;
            }
            ++i;
            continue;
        }

        fmt::print(stderr, "Warning: Unknown action at \"{}\"\n", ev.ts_event);
        ++i;
    }
}

void MessageWriter::PrintSummary() const {
    fmt::print("Message counts by type:\n");
    for (size_t t = 1; t < counts_.size(); ++t) {
        fmt::print("  Type {}: {}\n", t, counts_[t]);
    }
    fmt::print("Cleanup cancels skipped (partial T+F+C / F+C): {}\n", cleanup_cancels_skipped_);
    fmt::print("Cleanup full-fill occurrences (marked Deleted, no Delete emitted): {}\n",
        cleanup_fulfills_marked_deleted_);
}