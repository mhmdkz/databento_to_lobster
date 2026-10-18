//
// Created by M on 9/23/2025.
//

#ifndef BOOK_HPP
#define BOOK_HPP

#endif //BOOK_HPP

#pragma once
#include "lob_types.hpp"
#include <fstream>

class BookWriter {
public:
    BookWriter(std::ofstream& os, int depth);
    // Emit orderbook row using snapshot_ev. The caller (MessageWriter) decides
    // which event's post-state this is — normally the emitted event's own
    // post-state, except matched Trade+Fill+Cancel / Fill+Cancel pairs, which
    // intentionally use the paired Cancel's post-state instead.
    void EmitOrderbookRow(const lob::Event& snapshot_ev);

private:
    std::ofstream& out_;
    int depth_;
};