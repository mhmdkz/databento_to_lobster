// main.cpp

#include "lob_types.hpp"
#include "book.hpp"
#include "message.hpp"
#include "book_constructor.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <charconv>
#include <system_error>
#include <exception>

namespace fs = std::filesystem;

namespace {

    struct Ymd {
        int y;
        unsigned m;
        unsigned d;
    };

    std::optional<Ymd> ParseYmd(std::string_view s) {
        // expects "YYYY-MM-DD"
        if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;

        auto parse_int = [](std::string_view t) -> std::optional<int> {
            int v = 0;
            const char* b = t.data();
            const char* e = t.data() + t.size();
            auto [ptr, ec] = std::from_chars(b, e, v);
            if (ec != std::errc{} || ptr != e) return std::nullopt;
            return v;
            };

        auto y = parse_int(s.substr(0, 4));
        auto mo = parse_int(s.substr(5, 2));
        auto da = parse_int(s.substr(8, 2));
        if (!y || !mo || !da) return std::nullopt;
        if (*mo < 1 || *mo > 12) return std::nullopt;
        if (*da < 1 || *da > 31) return std::nullopt;

        return Ymd{ *y, static_cast<unsigned>(*mo), static_cast<unsigned>(*da) };
    }

    // US/Eastern DST rule (Energy Policy Act 2005):
    // starts: 2nd Sunday in March
    // ends:   1st Sunday in November
    bool IsUsEasternDstDate(const Ymd& d) {
        using namespace std::chrono;

        const year yy{ d.y };
        const month mar{ 3 };
        const month nov{ 11 };

        const sys_days mar1 = sys_days{ yy / mar / day{1} };
        const sys_days nov1 = sys_days{ yy / nov / day{1} };

        auto first_sunday_dom = [](sys_days first_of_month) -> unsigned {
            // weekday::c_encoding(): Sunday=0 ... Saturday=6
            const unsigned w = weekday{ first_of_month }.c_encoding();
            return 1u + ((7u - w) % 7u); // day-of-month of first Sunday
            };

        const unsigned mar_first_sun = first_sunday_dom(mar1);
        const unsigned dst_start_dom = mar_first_sun + 7u;   // 2nd Sunday in March
        const unsigned dst_end_dom = first_sunday_dom(nov1); // 1st Sunday in Nov

        const sys_days dst_start = sys_days{ yy / mar / day{dst_start_dom} };
        const sys_days dst_end = sys_days{ yy / nov / day{dst_end_dom} };
        const sys_days cur = sys_days{ yy / month{d.m} / day{d.d} };

        // DST in effect for dates in [dst_start, dst_end)
        return (cur >= dst_start) && (cur < dst_end);
    }

    static int ParsePositiveIntOrDefault(const char* s, int def) {
        if (s == nullptr) return def;
        int v = def;
        const char* end = s + std::char_traits<char>::length(s);
        auto [ptr, ec] = std::from_chars(s, end, v);
        if (ec != std::errc{} || ptr != end || v <= 0) return def;
        return v;
    }

    static bool EndsWith(std::string_view s, std::string_view suffix) {
        return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
    }

    static std::string BuildOutputPrefix(const std::string& out_name,
        const std::string& out_date,
        const std::string& session_tag) {
        return fmt::format("{}_{}_{}", out_name, out_date, session_tag);
    }

} // namespace

int main(int argc, char** argv) {
    // Base directories
    const fs::path databento_root = "E:/databento/";
    const fs::path output_root = "E:/CPP_projects/MSVS/new_db_lobster";


    const std::string session_tag = "34200000_57600000";

    // Defaults (used if args are missing)
    std::string input_token = "DEMO2025JAN/xnas-itch-20250122.mbo.GOOGL.dbn";
    std::string out_name = "GOOGL";
    std::string out_date = "2025-01-22";
    int depth = 1;

    // CLI:
    // <relative_dbn_without_.zst OR full_path> <NAME> <YYYY-MM-DD> <DEPTH>
    if (argc >= 2) input_token = argv[1];
    if (argc >= 3) out_name = argv[2];
    if (argc >= 4) out_date = argv[3];
    if (argc >= 5) depth = ParsePositiveIntOrDefault(argv[4], depth);

    if (const auto ymd = ParseYmd(out_date)) {
        const bool is_dst = IsUsEasternDstDate(*ymd);
        lob::SetTzOffsetHours(is_dst ? 4 : 5);
    }
    else {
        // If date parsing fails, keep default (EDT=4) to preserve behavior.
    }

    fmt::print("🕒 US/Eastern offset set to UTC-{} based on date {}\n",
        (lob::TZ_OFFSET_SECONDS / 3600), out_date);

    if (depth <= 0) {
        fmt::print(stderr, "❌ Invalid depth: {}\n", depth);
        return 1;
    }

    // Build full input path:
    // - If absolute path given: use it as-is
    // - Else prefix with databento_root
    // - If it doesn't end with .zst, append .zst
    fs::path input_path = fs::path(input_token);
    if (!input_path.is_absolute() && !input_path.has_root_name()) {
        input_path = databento_root / input_path;
    }

    std::string input_str = input_path.generic_string();
    if (!EndsWith(input_str, ".zst")) {
        input_str += ".zst";
        input_path = fs::path(input_str);
    }

    if (!fs::exists(input_path)) {
        fmt::print(stderr, "❌ Input file not found:\n  {}\n", input_path.string());
        return 1;
    }

    // Output paths
    const std::string prefix = BuildOutputPrefix(out_name, out_date, session_tag);
    const std::string depth_str = std::to_string(depth);

    const fs::path msg_path = output_root / fmt::format("{}_message_{}.csv", prefix, depth_str);
    const fs::path book_path = output_root / fmt::format("{}_orderbook_{}.csv", prefix, depth_str);

    // Ensure output directory exists
    std::error_code ec;
    fs::create_directories(msg_path.parent_path(), ec);
    if (ec) {
        fmt::print(stderr, "⚠ Could not ensure output directory exists: {}\n", ec.message());
    }

    std::ofstream msg_out(msg_path, std::ios::out | std::ios::trunc);
    std::ofstream book_out(book_path, std::ios::out | std::ios::trunc);
    if (!msg_out.is_open() || !book_out.is_open()) {
        fmt::print(stderr, "❌ Failed to open output files:\n  {}\n  {}\n",
            msg_path.string(), book_path.string());
        return 1;
    }

    // Build events into RAM (replays DBN and constructs events)
    bc::EventBuffer buf;
    try {
        buf = bc::BuildEventsFromDbnFile(input_path.string(), depth, /*reserve_hint=*/0);
    }
    catch (const std::exception& ex) {
        fmt::print(stderr, "❌ Failed to build events from input: {}\n", ex.what());
        return 1;
    }

    if (buf.depth != depth) {
        fmt::print(stderr, "⚠ Builder returned depth {} but requested {}. Using builder depth.\n",
            buf.depth, depth);
        depth = buf.depth;
    }

    fmt::print("📊 Input: {}\n", input_path.string());
    fmt::print("📊 Builder produced {} events, depth = {}\n", buf.events.size(), depth);
    fmt::print("📝 Writing:\n  {}\n  {}\n", msg_path.string(), book_path.string());

    // Wire up writers
    BookWriter book_writer(book_out, depth);
    MessageWriter message_writer(msg_out, book_writer, depth);

    // Process events in order; group events by identical sequence number
    // (i.e. all sub-records normalized from one logical TotalView message —
    // e.g. Trade+Fill+Cancel for an Order Executed event). Databento only
    // guarantees these sub-records share `sequence`, not `ts_event`, so
    // grouping must key on sequence to reliably keep such sub-records together.
    std::vector<lob::Event> group;
    group.reserve(32);

    std::size_t rows_total = 0;
    std::size_t groups_total = 0;
    std::size_t emitted_groups = 0;

    auto flush_group = [&](std::vector<lob::Event>& g) {
        if (g.empty()) return;
        ++groups_total;
        message_writer.ProcessGroup(g);
        ++emitted_groups;
        g.clear();
        };

    for (auto& e : buf.events) {
        ++rows_total;
        lob::Event ev = std::move(e);

        if (group.empty() || ev.sequence == group.front().sequence) {
            // Canary: confirms empirically whether ts_event ever diverges within
            // one sequence on real data. Safe to remove once validated.
            if (!group.empty() && ev.ts_event != group.front().ts_event) {
                fmt::print(stderr, "⚠ sequence {} spans differing ts_event: {} vs {}\n",
                    ev.sequence, group.front().ts_event, ev.ts_event);
            }
            group.push_back(std::move(ev));
        }
        else {
            flush_group(group);
            group.push_back(std::move(ev));
        }
    }

    flush_group(group);

    msg_out.flush();
    book_out.flush();

    if (!msg_out.good() || !book_out.good()) {
        fmt::print(stderr, "⚠ Output stream reported an error while writing files.\n");
        return 1;
    }

    fmt::print("=== Summary ===\n");
    fmt::print("Events processed: {}\n", rows_total);
    fmt::print("Timestamp groups: {}\n", groups_total);
    fmt::print("Groups emitted: {}\n", emitted_groups);
    fmt::print("Done. Wrote '{}' and '{}'\n", msg_path.string(), book_path.string());

    message_writer.PrintSummary();
    return 0;
}