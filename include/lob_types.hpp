//
// Created by M on 9/23/2025.
//

#ifndef LOB_TYPES_HPP
#define LOB_TYPES_HPP

#endif //LOB_TYPES_HPP


#pragma once
// Shared types & small parsing utilities used across the project.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <charconv>
#include <cmath>   // llround
#include <cstring> // memcpy

namespace lob {

    // basic typedefs
    using u64 = uint64_t;
    using i64 = int64_t;

    // timezone offset (subtract 4 hours as requested). Change if needed.
      // timezone offset in seconds (US/Eastern). Default = EDT (UTC-4).
      // Runtime-configurable so main.cpp can set EDT/EST based on date.
    inline int TZ_OFFSET_SECONDS{ 4 * 3600 };

    inline void SetTzOffsetHours(int hours) noexcept {
        TZ_OFFSET_SECONDS = hours * 3600;
    }


    // Actions and sides
    enum class Action { Add, Cancel, Modify, Trade, Fill, NoneAct, Unknown };
    enum class Side { Bid, Ask, NoneSide, UnknownSide };

    // Event & OrderState
    struct Event {
        std::string ts_event;             // "HH:MM:SS.NNN..."
        Action action{ Action::Unknown };
        Side side{ Side::UnknownSide };
        u64 order_id{ 0 };
        double price{ 0.0 };
        u64 size{ 0 };
        uint32_t sequence{ 0 };              // venue-assigned sequence number (from MboMsg::sequence)
        // used to group sub-records of one logical TotalView
        // message (e.g. Trade+Fill+Cancel); NOT written to
        // LOBSTER output, internal grouping key only.
        std::vector<double> ask_px;
        std::vector<u64>   ask_sz;
        std::vector<double> bid_px;
        std::vector<u64>   bid_sz;
    };

    // OrderState now holds a status/state for the order-pool strategy,
    // plus tracing fields for lifecycle monitoring.
    struct OrderState {
        enum class Status { Active, ClosedWaitingCancel, Deleted };

        u64 remaining{ 0 };                  // remaining size
        u64 original_size{ 0 };              // size from first Add
        Side side{ Side::UnknownSide };
        double price{ 0.0 };
        Status status{ Status::Active };

        // tracing fields
        std::string first_seen_ts;         // first Add timestamp
        std::string last_seen_ts;          // last event timestamp
        u64 total_filled{ 0 };               // cumulative fills
        u64 total_canceled{ 0 };             // cumulative cancels
    };

    // trimming helper
    [[nodiscard]]
    inline std::string_view Trim(std::string_view s) noexcept {
        const char* data = s.data();
        std::size_t len = s.size();

        std::size_t i = 0;
        while (i < len && (data[i] == ' ' || data[i] == '\t')) ++i;

        std::size_t j = len;
        while (j > i && (data[j - 1] == ' ' || data[j - 1] == '\t')) --j;

        return std::string_view{ data + i, j - i }; // create a fresh view object
    }

    // Parse Action from string_view
    inline Action ParseAction(std::string_view s) {
        if (s == "Add") return Action::Add;
        if (s == "Cancel") return Action::Cancel;
        if (s == "Modify") return Action::Modify;
        if (s == "Trade") return Action::Trade;
        if (s == "Fill") return Action::Fill;
        if (s == "None") return Action::NoneAct;
        return Action::Unknown;
    }

    // Parse Side from string_view
    inline Side ParseSide(std::string_view s) {
        if (s == "Bid") return Side::Bid;
        if (s == "Ask") return Side::Ask;
        if (s == "None") return Side::NoneSide;
        return Side::UnknownSide;
    }

    // Parse unsigned integer
    inline bool ParseU64(std::string_view s, u64& out) {
        std::string_view sv = Trim(s);
        const char* p = sv.data();
        const char* e = p + sv.size();
        u64 val = 0;
        auto res = std::from_chars(p, e, val);
        if (res.ec == std::errc() || res.ptr == e) {
            out = val;
            return true;
        }
        return false;
    }

    // Parse double (fallback to strtod)
    inline bool ParseDouble(std::string_view s, double& out) {
        auto sv = Trim(s);
        errno = 0;
        char buf[64];
        size_t n = sv.size() < sizeof(buf) - 1 ? sv.size() : sizeof(buf) - 1;
        memcpy(buf, sv.data(), n);
        buf[n] = '\0';
        char* endptr = nullptr;
        double v = std::strtod(buf, &endptr);
        if (endptr == buf) return false;
        out = v;
        return true;
    }

    // Price conversion to LOBSTER integer (price * 10000)
    inline long long PriceToLobsterInt(double px) {
        return static_cast<long long>(std::llround(px * 10000.0));
    }

    // Parse timestamp: "HH:MM:SS.NNNNNNNNN" -> seconds since midnight as double,
    // then subtract TZ_OFFSET_SECONDS.
    inline bool ParseTsToSeconds(std::string_view ts, double& out_seconds) {
        std::string_view left = ts;
        std::string_view frac;
        auto dot_pos = ts.find('.');
        if (dot_pos != std::string_view::npos) {
            left = ts.substr(0, dot_pos);
            frac = ts.substr(dot_pos + 1);
        }

        auto p1 = left.find(':');
        if (p1 == std::string_view::npos) return false;
        auto p2 = left.find(':', p1 + 1);
        if (p2 == std::string_view::npos) return false;

        int hh = 0, mm = 0, ss = 0;
        int tmp = 0;
        auto part = left.substr(0, p1);
        tmp = 0; for (char c : part) if (c >= '0' && c <= '9') tmp = tmp * 10 + (c - '0'); hh = tmp;
        part = left.substr(p1 + 1, p2 - p1 - 1); tmp = 0; for (char c : part) if (c >= '0' && c <= '9') tmp = tmp * 10 + (c - '0'); mm = tmp;
        part = left.substr(p2 + 1); tmp = 0; for (char c : part) if (c >= '0' && c <= '9') tmp = tmp * 10 + (c - '0'); ss = tmp;

        long long nanos = 0;
        if (!frac.empty()) {
            int digits = static_cast<int>(frac.size());
            if (digits > 9) digits = 9;
            long long v = 0;
            for (int i = 0;i < digits;++i) {
                char c = frac[i];
                if (c < '0' || c > '9') break;
                v = v * 10 + (c - '0');
            }
            if (static_cast<int>(frac.size()) < 9) {
                for (int k = 0; k < 9 - static_cast<int>(frac.size()); ++k)
                    v *= 10;
            }
            nanos = v;
        }

        double seconds = static_cast<double>(hh * 3600 + mm * 60 + ss) + static_cast<double>(nanos) / 1e9;
        seconds -= static_cast<double>(TZ_OFFSET_SECONDS);
        out_seconds = seconds;
        return true;
    }

    // Simple CSV splitter that handles quoted fields and "" escapes.
    // Returns vector<string> (copies).
    inline std::vector<std::string> SplitCsvLine(const std::string& line) {
        std::vector<std::string> out;
        out.reserve(32);
        const char* p = line.c_str();
        const char* end = p + line.size();
        while (p < end) {
            if (*p == '"') {
                ++p;
                const char* start = p;
                std::string token;
                while (p < end) {
                    if (*p == '"') {
                        if (p + 1 < end && *(p + 1) == '"') {
                            token.append(start, p - start);
                            token.push_back('"');
                            p += 2;
                            start = p;
                            continue;
                        }
                        else {
                            token.append(start, p - start);
                            ++p;
                            break;
                        }
                    }
                    ++p;
                }
                while (p < end && *p != ',') ++p;
                if (p < end && *p == ',') ++p;
                out.push_back(std::move(token));
            }
            else {
                const char* start = p;
                while (p < end && *p != ',') ++p;
                out.emplace_back(start, p - start);
                if (p < end && *p == ',') ++p;
            }
        }
        return out;
    }

} // namespace lob