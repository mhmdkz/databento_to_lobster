// book_constructor.cpp
// Implementation: replay Databento DBN.ZST and build lob::Event objects in RAM.

#include "book_constructor.hpp"

#include <databento/constants.hpp>   // db::kUndefPrice
#include <databento/datetime.hpp>    // db::ToIso8601
#include <databento/dbn_file_store.hpp>
#include <databento/enums.hpp>       // db::Action, db::Side
#include <databento/record.hpp>      // db::Record, db::MboMsg
#include <databento/symbol_map.hpp>

#include <fmt/format.h>

#include <fstream>

#include <map>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>

#include <cmath>
#include <iterator>

namespace bc {
    namespace db = databento;

    // ---------------------------
    // Small helper types & Book
    // ---------------------------
    // These implement the same logic used to compute ask/bid levels previously
    // written to CSV. They must be defined before BuildEventsFromDbnFile so the
    // function can instantiate Market.

    struct PriceLevel {
        int64_t price{ db::kUndefPrice };
        uint32_t size{ 0 };
        uint32_t count{ 0 };
        [[nodiscard]] bool IsEmpty() const { return price == db::kUndefPrice; }
        explicit operator bool() const { return !IsEmpty(); }
    };

    class Book {
    public:
        // return ask level idx (0 = best)
        PriceLevel GetAskLevel(std::size_t idx = 0) const {
            if (offers_.size() <= idx) return {};
            auto it = offers_.begin();
            std::advance(it, idx);
            PriceLevel pl;
            pl.price = it->first;
            pl.size = it->second.size;
            return pl;
        }

        // return bid level idx (0 = best)
        PriceLevel GetBidLevel(std::size_t idx = 0) const {
            if (bids_.size() <= idx) return {};
            auto it = bids_.rbegin();
            std::advance(it, idx);
            PriceLevel pl;
            pl.price = it->first;
            pl.size = it->second.size;
            return pl;
        }

        void Apply(const db::MboMsg& mbo) {
            switch (mbo.action) {
            case db::Action::Clear: Clear(); break;
            case db::Action::Add: Add(mbo); break;
            case db::Action::Cancel: Cancel(mbo); break;
            case db::Action::Modify: Modify(mbo); break;
            case db::Action::Trade:
            case db::Action::Fill:
            case db::Action::None:
                // Trade/Fill/None do not alter aggregated level sizes here.
                break;
            default:
                break;
            }
        }

    private:
        struct LevelInfo { uint32_t size{ 0 }; /* count omitted */ };

        void Clear() {
            orders_by_id_.clear();
            offers_.clear();
            bids_.clear();
        }

        void Add(const db::MboMsg& mbo) {
            if (mbo.flags.IsTob()) {
                // top-of-book: clear the side and optionally insert a level if price != undef
                if (mbo.side == db::Side::Ask) {
                    offers_.clear();
                    if (mbo.price != db::kUndefPrice) offers_.emplace(mbo.price, LevelInfo{ mbo.size });
                }
                else if (mbo.side == db::Side::Bid) {
                    bids_.clear();
                    if (mbo.price != db::kUndefPrice) bids_.emplace(mbo.price, LevelInfo{ mbo.size });
                }
            }
            else {
                if (mbo.side == db::Side::Ask) {
                    offers_[mbo.price].size += mbo.size;
                }
                else if (mbo.side == db::Side::Bid) {
                    bids_[mbo.price].size += mbo.size;
                }
                orders_by_id_[mbo.order_id] = { mbo.price, mbo.side, mbo.size };
            }
        }

        void Cancel(const db::MboMsg& mbo) {
            if (mbo.side == db::Side::Ask) {
                auto it = offers_.find(mbo.price);
                if (it != offers_.end()) {
                    if (it->second.size > mbo.size) it->second.size -= mbo.size;
                    else offers_.erase(it);
                }
            }
            else if (mbo.side == db::Side::Bid) {
                auto it = bids_.find(mbo.price);
                if (it != bids_.end()) {
                    if (it->second.size > mbo.size) it->second.size -= mbo.size;
                    else bids_.erase(it);
                }
            }
            orders_by_id_.erase(mbo.order_id);
        }

        void Modify(const db::MboMsg& mbo) {
            auto oit = orders_by_id_.find(mbo.order_id);
            if (oit != orders_by_id_.end()) {
                // subtract previous contribution
                auto prev_price = oit->second.price;
                auto prev_side = oit->second.side;
                auto prev_size = oit->second.size;
                if (prev_side == db::Side::Ask) {
                    auto it = offers_.find(prev_price);
                    if (it != offers_.end()) {
                        if (it->second.size > prev_size) it->second.size -= prev_size;
                        else offers_.erase(it);
                    }
                }
                else {
                    auto it = bids_.find(prev_price);
                    if (it != bids_.end()) {
                        if (it->second.size > prev_size) it->second.size -= prev_size;
                        else bids_.erase(it);
                    }
                }
                // add new contribution
                if (mbo.side == db::Side::Ask) {
                    offers_[mbo.price].size += mbo.size;
                }
                else {
                    bids_[mbo.price].size += mbo.size;
                }
                // update order record
                oit->second.price = mbo.price;
                oit->second.side = mbo.side;
                oit->second.size = mbo.size;
            }
            else {
                // if unknown order, treat as add (mirrors original defensive logic)
                Add(mbo);
            }
        }

        struct OrderRec { int64_t price; db::Side side; uint32_t size; };
        std::unordered_map<uint64_t, OrderRec> orders_by_id_;
        std::map<int64_t, LevelInfo> offers_;
        std::map<int64_t, LevelInfo> bids_;
    };

    class Market {
    public:
        Book& GetOrCreateBook(uint32_t instrument_id) {
            return books_[instrument_id];
        }

        void Apply(const db::MboMsg& mbo) {
            GetOrCreateBook(mbo.hd.instrument_id).Apply(mbo);
        }

        const Book& GetBook(uint32_t instrument_id) const {
            auto it = books_.find(instrument_id);
            if (it == books_.end()) {
                static const Book empty;
                return empty;
            }
            return it->second;
        }

    private:
        std::unordered_map<uint32_t, Book> books_;
    };

    // ---------------------------
    // Mapping helpers
    // ---------------------------
    static lob::Action MapAction(db::Action a) {
        switch (a) {
        case db::Action::Add: return lob::Action::Add;
        case db::Action::Cancel: return lob::Action::Cancel;
        case db::Action::Modify: return lob::Action::Modify;
        case db::Action::Trade: return lob::Action::Trade;
        case db::Action::Fill: return lob::Action::Fill;
        case db::Action::None: return lob::Action::NoneAct;
        default: return lob::Action::Unknown;
        }
    }

    static lob::Side MapSide(db::Side s) {
        switch (s) {
        case db::Side::Bid: return lob::Side::Bid;
        case db::Side::Ask: return lob::Side::Ask;
        case db::Side::None: return lob::Side::NoneSide;
        default: return lob::Side::UnknownSide;
        }
    }

    // Round databento nano dollar price to two decimal places as the CSV did.
    static std::optional<double> PriceToRoundedDollarsDouble(int64_t price) {
        if (price == db::kUndefPrice) return std::nullopt;
        double dollars = static_cast<double>(price) / 1e9;
        double rounded = std::round(dollars * 100.0) / 100.0;
        return rounded;
    }

    // ---------------------------
    // Main builder implementation
    // ---------------------------
    EventBuffer BuildEventsFromDbnFile(const std::string& dbn_path, int depth, size_t reserve_hint) {
        if (depth <= 0) throw std::invalid_argument("depth must be > 0");
        EventBuffer buf;
        buf.depth = depth;
        if (reserve_hint) buf.events.reserve(reserve_hint);

        // quick existence check
        if (!std::ifstream(dbn_path).good()) {
            throw std::runtime_error("input DBN file not found: " + dbn_path);
        }

        Market market;
        db::TsSymbolMap symbol_map;

        auto metadata_cb = [&symbol_map](const db::Metadata& md) {
            symbol_map = md.CreateSymbolMap();
            };

        // Preserve original flag behavior: produce row for each MBO unless only_on_last true
        constexpr bool only_on_last = false;

        auto record_cb = [&buf, &market, depth](const db::Record& record) -> db::KeepGoing {
            if (auto* mbo = record.GetIf<db::MboMsg>()) {
                // apply update to market/book
                market.Apply(*mbo);

                if constexpr (!only_on_last || mbo->flags.IsLast()) {
                    // timestamp iso -> time part
                    const auto iso = db::ToIso8601(mbo->hd.ts_event); // 2024-04-03T09:30:00.123456789Z
                    std::string ts_event;
                    const auto t_pos = iso.find('T');
                    if (const auto z_pos = iso.rfind('Z'); t_pos != std::string::npos && z_pos != std::string::npos && z_pos >
                        t_pos) {
                        ts_event = iso.substr(t_pos + 1, z_pos - (t_pos + 1));
                    }
                    else {
                        ts_event = iso;
                    }

                    // Price guard: preserve original behavior: if price undefined skip row
                    if (mbo->price == db::kUndefPrice) {
                        return db::KeepGoing::Continue; // skip
                    }

                    const auto price_opt = PriceToRoundedDollarsDouble(mbo->price);
                    if (!price_opt.has_value()) return db::KeepGoing::Continue;

                    lob::Event ev;
                    ev.ts_event = std::move(ts_event);
                    ev.action = MapAction(mbo->action);
                    ev.side = MapSide(mbo->side);
                    ev.order_id = static_cast<lob::u64>(mbo->order_id);
                    ev.price = price_opt.value(); // e.g., 123.45
                    ev.size = static_cast<lob::u64>(mbo->size);
                    ev.sequence = mbo->sequence; // venue sequence number — used by main.cpp to group T+F+C sub-records correctly

                    // allocate and fill levels
                    ev.ask_px.assign(depth, 0.0);
                    ev.ask_sz.assign(depth, 0);
                    ev.bid_px.assign(depth, 0.0);
                    ev.bid_sz.assign(depth, 0);

                    const Book& book = market.GetBook(mbo->hd.instrument_id);
                    for (int lvl = 0; lvl < depth; ++lvl) {
                        if (const auto ask = book.GetAskLevel(static_cast<std::size_t>(lvl))) {
                            if (auto p_opt = PriceToRoundedDollarsDouble(ask.price); p_opt.has_value()) ev.ask_px[lvl] = p_opt.value();
                            ev.ask_sz[lvl] = ask.size;
                        }
                        if (const auto bid = book.GetBidLevel(static_cast<std::size_t>(lvl))) {
                            auto p_opt = PriceToRoundedDollarsDouble(bid.price);
                            if (p_opt.has_value()) ev.bid_px[lvl] = p_opt.value();
                            ev.bid_sz[lvl] = bid.size;
                        }
                    }

                    buf.events.emplace_back(std::move(ev));
                }
            }
            return db::KeepGoing::Continue;
            };

        try {
            db::DbnFileStore store{ dbn_path };
            store.Replay(metadata_cb, record_cb);
        }
        catch (const std::exception& ex) {
            throw std::runtime_error(std::string("error replaying DBN file: ") + ex.what());
        }

        return buf;
    }

} // namespace bc