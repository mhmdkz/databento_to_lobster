//
// Created by M on 9/23/2025.
//

#include "book.hpp"
#include "lob_types.hpp"
#include <fmt/format.h>

BookWriter::BookWriter(std::ofstream &os, int depth) : out_(os), depth_(depth) {}

void BookWriter::EmitOrderbookRow(const lob::Event &snapshot_ev) {
    const long long ASK_EMPTY = 9999999999LL;
    const long long BID_EMPTY = -9999999999LL;

    for (int i = 0; i < depth_; ++i) {
        // ask px
        if (i < static_cast<int>(snapshot_ev.ask_px.size()) && snapshot_ev.ask_px[i] != 0.0) {
            long long apx = lob::PriceToLobsterInt(snapshot_ev.ask_px[i]);
            out_ << apx;
        } else {
            out_ << ASK_EMPTY;
        }
        out_ << ',';
        // ask size
        if (i < static_cast<int>(snapshot_ev.ask_sz.size())) out_ << snapshot_ev.ask_sz[i];
        else out_ << 0;
        out_ << ',';
        // bid px
        if (i < static_cast<int>(snapshot_ev.bid_px.size()) && snapshot_ev.bid_px[i] != 0.0) {
            long long bpx = lob::PriceToLobsterInt(snapshot_ev.bid_px[i]);
            out_ << bpx;
        } else {
            out_ << BID_EMPTY;
        }
        out_ << ',';
        // bid size
        if (i < static_cast<int>(snapshot_ev.bid_sz.size())) out_ << snapshot_ev.bid_sz[i];
        else out_ << 0;

        if (i + 1 < depth_) out_ << ',';
    }
    out_ << '\n';
}
