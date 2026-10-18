//
// Created by M on 10/10/2025.
//

#ifndef BOOK_CONSTRUCTOR_HPP
#define BOOK_CONSTRUCTOR_HPP

#endif //BOOK_CONSTRUCTOR_HPP

#pragma once

// book_constructor.hpp
// Build an in-memory sequence of lob::Event by replaying Databento DBN.ZST files.
//
// Usage:
//   bc::EventBuffer buf = bc::BuildEventsFromDbnFile(dbn_path, depth, reserve_hint);
//   // then iterate buf.events in order

#include "lob_types.hpp"
#include <string>
#include <vector>

namespace bc {

    struct EventBuffer {
        std::vector<lob::Event> events;
        int depth = 0; // number of LOB levels per side used when constructing events
    };

    // Build events from a databento DBN.ZST file. `depth` is required
    EventBuffer BuildEventsFromDbnFile(const std::string& dbn_path, int depth, size_t reserve_hint = 0);

} // namespace bc

