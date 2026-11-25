// The AlphaWOTR bot on the module (see bot.h), as protocol/wotr_protocol_bot.py reads.
#include "bot.h"
#include "graph_text.h"
#include "opts.h"
#include "wotr_random.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <set>

extern "C" int64_t awasm_snapshot_root_count(const void* handle);

using awjson::Value;
using namespace awad::opt;

#ifndef AW_COMMIT
#define AW_COMMIT "unknown"
#endif

namespace awbot {
namespace {
const std::set<std::string> CHAIN_KINDS = {"ARMY_MOVE", "RECRUIT", "MOVE_COMPANIONS", "MOVE_MINIONS", "CARD_CHOICE",
                                           "JOIN_GROUP", "ARMY_SELECTION", "CHOOSE_ATTACK", "SEPARATE_COMPANIONS"};
const std::set<std::string> SMALL_KINDS = {"discard_card", "discard_table_card", "eye_allocate", "fellowship_declare", "declare_decline",
                                           "combat_retreat", "combat_no_retreat", "retreat_into_siege", "casualty_allocate",
                                           "casualty_preserve_units", "casualty_preserve_elites"};
constexpr int64_t SMALL_SIMS = 100;
const std::set<std::string> SETUP_KINDS = {"CARD_CHOICE", "ACTION", "FELLOWSHIP_PHASE", "HUNT_ALLOCATION", "DISCARD_EXCESS_CARD", "COMBAT_CARD",
                                           "CASUALTY", "CEASE_ATTACK", "EXTEND_SIEGE", "RETREAT", "FIELD_OR_SIEGE", "ADVANCE", "WHITE_RIDER",
                                           "HUNT_CASUALTY", "GUIDE_ABILITY", "REVEAL_FELLOWSHIP", "CHOOSE_GUIDE"};
std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
std::string json_of(const Value& v) {
    switch (v.kind) {
    case Value::Null: return "null";
    case Value::Bool: return v.b ? "true" : "false";
    case Value::Int: return std::to_string(v.i);
    case Value::Real: { char b[32]; std::snprintf(b, sizeof b, "%.17g", v.d); return b; }
    case Value::Str: return json_str(v.s);
    case Value::Arr: { std::string o = "["; for (size_t i = 0; i < v.arr.size(); ++i) o += (i ? "," : "") + json_of(v.arr[i]); return o + "]"; }
    case Value::Obj: { std::string o = "{"; for (size_t i = 0; i < v.obj.size(); ++i) o += (i ? "," : "") + json_str(v.obj[i].first) + ":" + json_of(v.obj[i].second); return o + "}"; }
    }
    return "null";
}
std::string row_type(const pyobj::Graph& g, uint64_t row) {
    awgt::G a{g};
    return a.text(a.kids(row)[1]);
}
}  // namespace

std::optional<double> decision_ms(const Value& clock, const std::string& clock_kind) {
    const Value* left = clock.get("left_ms");
    if (!left || left->is_null()) return std::nullopt;
    if (clock_kind == "correspondence" || clock.get("deadline")) return std::nullopt;
    const double l = left->kind == Value::Real ? left->d : double(left->as_int());
    if (clock_kind == "game" || clock.get("opponent_ms")) {
        const Value* inc = clock.get("increment_ms");
        const double i = (inc && inc->truthy()) ? (inc->kind == Value::Real ? inc->d : double(inc->as_int())) : 0.0;
        return std::min(l / 16.0 + i / 2.0, l / 4.0);
    }
    return l;
}

Bot::Bot(pyobj::Model& model, awad::Adapter& adapter, const awad::Tables& tables, awdec::Box& b)
    : m(model), ad(adapter), t(tables), box(b) {}
Bot::~Bot() { if (memo.rng) wotr_random_destroy(memo.rng); if (own_rng) wotr_random_destroy(own_rng); }

void Bot::set_memo_rng(const std::vector<uint32_t>& words, uint32_t index, bool has_gauss, double gauss) {
    uint64_t bits = 0; std::memcpy(&bits, &gauss, 8);
    if (memo.rng) wotr_random_destroy(memo.rng);
    memo.rng = wotr_random_create(words.data(), words.size(), index, has_gauss ? 1 : 0, bits);
}

// LeagueSearcher.budget (branch ours, b2445d1): from 5 s a decision 80% of the clock less 0.3 s, under 5 s a quarter
// less 0.1 s; the first search of a kind at most half the clock (from 5 s); a kind not timed yet takes the slowest
// measured rate or FIRST_RATE (60 a second), never a faster kind's
int64_t Bot::budget(const std::string& kind, std::optional<double> left_ms) const {
    constexpr double FIRST_RATE = 60.0;
    int64_t n = sims;
    std::optional<double> limit = max_seconds;
    if (left_ms) {
        const double left = *left_ms / 1000.0;
        double room = left >= 5.0 ? std::max(0.5, left * 0.8 - 0.3) : std::max(0.25, left * 0.25 - 0.1);
        if (left >= 5.0 && !rates.count(kind)) room = std::min(room, std::max(0.5, left * 0.5 - 0.2));
        limit = limit ? std::min(*limit, room) : room;
    }
    if (limit) {
        auto it = rates.find(kind);
        double rate = FIRST_RATE;
        if (it != rates.end() && it->second) rate = it->second;
        else for (const auto& [k, r] : rates) rate = std::min(rate, r);
        n = std::min<int64_t>(n, std::max<int64_t>(8, int64_t(rate * *limit)));
    }
    return n;
}

std::string Bot::handle(const Value& msg) {
    const std::string ty = at(msg, "type").kind == Value::Str ? at(msg, "type").s : "";
    if (ty == "new_game") {
        const Value& sb = at(msg, "seed_bot");
        const uint64_t seed = sb.truthy() ? uint64_t(std::llabs(sb.as_int())) : 0;
        if (memo.rng) wotr_random_destroy(memo.rng);
        memo = awad::Memo();
        memo.side = at(msg, "side").kind == Value::Str && at(msg, "side").s == "SHADOW" ? 1 : 0;
        const uint32_t limbs[2] = {uint32_t(seed), uint32_t(seed >> 32)};
        memo.rng = wotr_random_create_seeded(limbs, (seed >> 32) ? 2 : 1);   // random.Random(seed)
        own_seed = seed;
        if (own_rng) wotr_random_destroy(own_rng);
        own_rng = wotr_random_create_seeded(limbs, (seed >> 32) ? 2 : 1);   // random.seed(seed_bot): the global stream
        plan.reset(); drop_work(); in_game = true;
        hows.clear();
        game = at(msg, "game").kind == Value::Str ? at(msg, "game").s : "";
        start_view.reset(); prefix.reset();
        if (at(msg, "start").kind == Value::Str && at(msg, "start").s == "position" && at(msg, "played_for_you").truthy()) {
            start_view = at(msg, "view");
            prefix = std::vector<Value>();
        }
        clock_kind = at(or_empty(msg, "clock"), "kind").kind == Value::Str ? at(or_empty(msg, "clock"), "kind").s : "";
        return "{\"type\":\"ready\",\"id\":" + json_of(at(msg, "id")) + "}";
    }
    if (ty == "hello")
        return "{\"type\":\"hello\",\"id\":" + json_of(at(msg, "id")) + ",\"protocol\":\"wotr-bot/0.1\",\"name\":\"AlphaWOTR\","
               "\"version\":\"v0.2.0/mc200_fpwin_shwin-s1/protocol-2/wasm-" AW_COMMIT "\",\"author\":\"AlphaWOTR (protocol adapter, WebAssembly)\",\"events\":true,"
               "\"options\":[{\"name\":\"sims\",\"type\":\"int\",\"default\":" + std::to_string(sims) + ",\"min\":1,\"max\":100000}]}";
    if (ty == "options") {
        const Value& v = or_empty(msg, "values");
        if (v.get("sims")) { try { sims = std::max<int64_t>(1, at(v, "sims").as_int()); } catch (...) {} }
        return "";
    }
    if (ty == "event") {   // GameMemo.event
        if (!in_game) return "";
        const Value& ev = or_empty(msg, "event");
        const std::string k = at(ev, "kind").kind == Value::Str ? at(ev, "kind").s : "";
        if (prefix && k == "choice") prefix->push_back(ev);
        if (k == "turn_started") { memo.messenger_used = false; memo.fellowship_acted = false; }
        else if (k == "fellowship_entered_mordor") memo.in_mordor = true;
        else if (k == "fellowship_moved" || k == "fellowship_hidden") { if (memo.in_mordor) memo.fellowship_acted = true; }
        else if (k == "die_used") {
            if (at(ev, "side").kind == Value::Str && at(ev, "side").s == "SHADOW" && at(ev, "face").kind == Value::Str && at(ev, "face").s == "MUSTER" &&
                at(ev, "action").kind == Value::Str && (at(ev, "action").s == "move_armies" || at(ev, "action").s == "attack"))
                memo.messenger_used = true;
        }
        // The Eagles are Coming!: the region of the Nazgul it struck (GameMemo.event, wotr #1431)
        if (k == "card_played") { memo.eagles = at(ev, "card").kind == Value::Str && at(ev, "card").s == "fp-char-18"; memo.eagles_from.clear(); }
        else if (k == "figure_eliminated" && memo.eagles && at(ev, "unit").kind == Value::Str && at(ev, "unit").s == "nazgul")
            memo.eagles_from = at(ev, "region").kind == Value::Str ? at(ev, "region").s : "";
        return "";
    }
    if (ty == "decide") return decide(msg);
    if (ty == "game_over") {
        in_game = false;
        if (report) {   // the Python adapter's game-over line: the answers by how
            int64_t n = 0; std::string parts;
            for (const auto& [h, c] : hows) { n += c; parts += (parts.empty() ? "" : ", ") + h + " " + std::to_string(c); }
            const int64_t defaults = (hows.count("default") ? hows.at("default") : 0) + (hows.count("error-default") ? hows.at("error-default") : 0);
            std::fprintf(stderr, "[aw] game over %s: %lld decisions (%s), %lld defaults\n", game.c_str(), (long long)n, parts.c_str(), (long long)defaults);
        }
        return "";
    }
    return "";
}

std::string Bot::decide(const Value& msg) {
    const Value& dec = at(msg, "decision");
    last = Trace();
    const auto left = decision_ms(or_empty(msg, "clock"), clock_kind);
    {   // AlphaWotrBot._deadline: a per-decision clock's answer deadline (MARGIN_MS 200)
        const Value& clock = or_empty(msg, "clock");
        const Value* l = clock.get("left_ms");
        answer_deadline.reset();
        if (l && !l->is_null() && clock_kind == "per_decision" && !clock.get("deadline") && !clock.get("opponent_ms")) {
            const double ms = l->kind == Value::Real ? l->d : double(l->as_int());
            answer_deadline = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(std::max(0.0, (ms - 200.0) / 1000.0)));
        }
    }
    std::optional<int64_t> o; std::string how;
    try { std::tie(o, how) = decide_(dec, or_empty(msg, "view"), left); }
    catch (const std::exception& e) { how = "error"; last.note = e.what(); o.reset(); }
    std::set<int64_t> legal;
    for (const Value& x : or_empty(dec, "options").arr) if (x.get("o") && !at(x, "o").is_null()) legal.insert(at(x, "o").as_int());
    if (!o || !legal.count(*o)) {
        const auto d = awad::default_option(dec);
        o = d.first;
        how = how != "error" ? "default" : "error-default";
        plan.reset();
    }
    last.o = o; last.how = how;
    ++hows[how];
    if (report && (how == "default" || how == "error-default"))
        std::fprintf(stderr, "[aw] %s %s %s: %s\n", how.c_str(), game.c_str(), at(dec, "kind").s.c_str(),
                     last.note.empty() ? (last.unported.empty() ? "no engine decision" : last.unported.c_str()) : last.note.c_str());
    return "{\"type\":\"choice\",\"id\":" + json_of(at(msg, "id")) + ",\"o\":" + std::to_string(*o) + ",\"fingerprint\":" +
           json_of(at(dec, "fingerprint")) + ",\"info\":{\"how\":" + json_str(how) + "}}";
}


// the engine's menu on a state: get_legal_actions (the box's full menu), project_ai_actions, each row's text
struct Menu {
    void* sn = nullptr; void* full = nullptr; void* sel = nullptr; void* meta = nullptr;
    pyobj::Graph mg;
    std::vector<uint64_t> rowsn;
    std::vector<std::string> rows, prow;
    ~Menu() { for (void* p : {sn, full, sel, meta}) if (p) wotr_snapshot_destroy(p); }
};

namespace {
std::unique_ptr<Menu> menu_of(pyobj::Model& m, pyobj::Obj* st, const void* obs, int side) {
    auto mn = std::make_unique<Menu>();
    const pyobj::Graph gf = m.encode(st);
    mn->sn = wotr_snapshot_create(gf.values.data(), gf.values.size(), gf.edges.data(), gf.edges.size(), gf.bytes.data(), gf.bytes.size(), gf.root);
    if (!mn->sn) throw std::runtime_error(std::string("state: ") + wotr_snapshot_last_error());
    mn->full = wotr_snapshot_full_actions(mn->sn, obs, side);
    if (!mn->full) throw std::runtime_error(std::string("menu: ") + wotr_snapshot_last_error());
    mn->rows = awgt::menu_rows(mn->full);
    mn->sel = wotr_snapshot_project_policy(mn->sn, mn->full, side, &mn->meta);
    if (mn->sel) mn->prow = awgt::menu_rows(mn->sel);
    if (mn->prow.empty()) mn->prow = mn->rows;
    mn->mg = awgt::export_graph(mn->full);
    awgt::G ga{mn->mg};
    mn->rowsn = ga.kids(mn->mg.root);
    return mn;
}
awad::Adapter::Row row_of(const Menu& mn, size_t ri) {
    awgt::G ga{mn.mg};
    const auto parts = ga.kids(mn.rowsn[ri]);
    awad::Adapter::Row row; row.die = mn.mg.values[parts[0]].integer; row.type = ga.text(parts[1]);
    row.params = awgt::to_value(ga, parts[2]); row.menu = mn.full; row.index = int64_t(ri); row.sn = mn.sn;
    return row;
}
bool is_none(pyobj::Obj* w, const char* name) {
    pyobj::Obj** v = pyobj::Model::attr_slot(w, name);
    return !v || (*v)->tag == WOTR_NONE;
}
std::string text_of(const Value& v) { return v.kind == Value::Str ? v.s : v.kind == Value::Null ? std::string("\x01") : v.kind == Value::Int ? std::to_string(v.i) : std::string("\x02"); }
}  // namespace

std::pair<std::optional<int64_t>, std::string> Bot::decide_(const Value& dec, const Value& view, std::optional<double> left) {
    const std::string kind = at(dec, "kind").s;
    const Value& options = or_empty(dec, "options");
    if (prefix) {   // the first question of a game from a position: the referee may have begun the bot's own action
        const std::vector<Value> pre = std::move(*prefix);
        prefix.reset();
        if (CHAIN_KINDS.count(kind)) {   // (half the clock: the question itself has the rest)
            const auto dl = answer_deadline;
            if (dl) { const auto now = std::chrono::steady_clock::now(); answer_deadline = now + (*dl > now ? (*dl - now) / 2 : std::chrono::steady_clock::duration(0)); }
            try { reenter(pre, left ? std::optional<double>(*left / 2.0) : std::nullopt); }
            catch (const std::exception& e) {
                if (report) std::fprintf(stderr, "[aw] ! %s: re-entering the played action: %s\n", game.c_str(), e.what());
                plan.reset(); drop_work();
            }
            answer_deadline = dl;
        }
    }
    if (options.arr.size() == 1) {
        if (plan && plan->takes(kind)) { try { plan->answer(dec); } catch (...) { plan.reset(); } }   // _continue_plan_quietly
        return {at(options.arr[0], "o").is_null() ? std::nullopt : std::optional<int64_t>(at(options.arr[0], "o").as_int()), "only"};
    }
    if (plan && plan->takes(kind)) {   // 1. the plan of the engine's last action
        const auto o = plan->answer(dec);
        if (o) { last.note = plan->name; return {o, "plan"}; }
    }
    plan.reset();
    if (work && CHAIN_KINDS.count(kind)) {   // 2. a chain of the engine's last action, on its copy of the state
        if (auto r = chain(dec, left)) return *r;
        if (auto o = diff_answer(dec)) { last.note = "as the engine resolved it"; return {o, "board"}; }
        if ((kind == "ARMY_MOVE" || kind == "RECRUIT" || kind == "MOVE_COMPANIONS" || kind == "MOVE_MINIONS") &&
            !opts(dec, "end").empty() && chain_over(work, kind)) {
            last.note = "chain over";
            const auto e = opts(dec, "end");
            return {has_o(e[0]) ? std::optional<int64_t>(at(*e[0], "o").as_int()) : std::nullopt, "chain"};
        }
    }
    if (!CHAIN_KINDS.count(kind)) drop_work();
    // 3. the state rebuilt from the view, the question set up
    pyobj::Obj* st = ad.build_state(view, memo);
    awad::Adapter::Setup su;
    if (SETUP_KINDS.count(kind)) {
        su = ad.setup(kind, st, dec, view, memo);
        if (!su.ported) { last.unported = "setup " + kind; return {std::nullopt, "unported"}; }
    }
    if (su.label.empty() && !su.has_match) {   // ready None: a rule, else the default
        bool has = false;
        const auto o = awad::rule(t, kind, dec, view, memo.side, work != nullptr, &has);
        if (o) return {o, "rule"};
        last.note = "no engine decision for " + kind;
        return {std::nullopt, "default"};
    }
    if (su.has_match) return {su.match_none ? std::nullopt : std::optional<int64_t>(su.match), "rule"};
    return search_and_map(st, dec, su.label, su.flag, left);
}

// LeagueSearcher.choose on a menu: the pick's row text ('' none); has_sims: its info carries 'sims';
// units_menu: the unit rule's action as a one-row menu (owned by the caller) when it named units
std::string Bot::choose(Menu& mn, const std::set<std::string>& banned, const std::string& label, std::optional<double> left,
                        bool& has_sims, void*& units_menu) {
    has_sims = false; units_menu = nullptr;
    const auto& rows = mn.rows;
    std::vector<int64_t> allowed;   // the projected menu's rows not banned (else the legal menu's)
    for (const auto& pr : mn.prow) if (!banned.count(pr)) { auto it = std::find(rows.begin(), rows.end(), pr); if (it != rows.end()) allowed.push_back(int64_t(it - rows.begin())); }
    if (allowed.empty()) for (size_t ri = 0; ri < rows.size(); ++ri) if (!banned.count(rows[ri])) allowed.push_back(int64_t(ri));
    if (allowed.empty()) { last.note = "no allowed action"; return ""; }
    has_sims = true;
    if (allowed.size() == 1) return rows[size_t(allowed[0])];
    int64_t n = budget(label, left);
    bool small = true;
    for (auto ai : allowed) small &= SMALL_KINDS.count(row_type(mn.mg, mn.rowsn[size_t(ai)])) != 0;
    if (n && small) n = std::min(n, SMALL_SIMS);
    std::string act;
    if (n) {
        const SearchArgs sa = next_search ? next_search(n) : SearchArgs{own_seed, uint32_t(last.searches), own_seed ^ 0x9e3779b97f4a7c15ULL, n};
        ++last.searches;
        const auto ts = std::chrono::steady_clock::now();
        // the menu as this decision worked it out (LeagueSearcher.choose's `legal`, passed on: worked out once)
        awdec::Projection given;
        if (speed_fixes && mn.sel) given = {mn.sel, mn.meta};
        auto pk = awdec::search(box, mn.sn, memo.side, sa.sims, sa.seat, sa.index, sa.seed, 0, given);
        if (!pk.error.empty()) throw std::runtime_error("search: " + pk.error);
        const auto mr = awgt::menu_rows(pk.menu);
        act = pk.index >= 0 && size_t(pk.index) < mr.size() ? mr[size_t(pk.index)] : "";
        awdec::release(pk);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
        last.search_s += secs;
        if (sa.sims >= 8 && secs > 0.05) {   // the rate: the slowest of the last three searches of this kind
            auto& rc = recent[label];
            if (rc.size() > 2) rc.erase(rc.begin(), rc.end() - 2);
            rc.push_back(sa.sims / secs);
            rates[label] = *std::min_element(rc.begin(), rc.end());
        }
    }
    std::string hit;
    for (auto ai : allowed) if (rows[size_t(ai)] == act) { hit = act; break; }
    if (hit.empty()) {   // the search's pick has no referee counterpart: the net's own choice among the allowed
        void* sub = awdec::menu_rows(mn.full, allowed);
        const int64_t ri = sub ? awdec::raw(box, mn.sn, memo.side, sub) : -1;
        if (sub) wotr_snapshot_destroy(sub);
        if (ri >= 0) hit = rows[size_t(allowed[size_t(ri)])];
    }
    std::string pick = hit;
    // THE CERTAIN LINE over the projected menu, then the ban check
    void* pmenu = (mn.sel && awasm_snapshot_root_count(mn.sel) > 0) ? mn.sel : mn.full;
    if (!hit.empty()) {
        int64_t own_i = -1;
        for (size_t q = 0; q < mn.prow.size(); ++q) if (mn.prow[q] == hit) { own_i = int64_t(q); break; }
        if (own_i >= 0) {
            // its time: this port's cap, and on a per-decision clock the answer's deadline less AFTER (40 ms)
            double seconds = speed_fixes ? certain_seconds : 0.0;
            if (speed_fixes && answer_deadline) {
                const double room = std::chrono::duration<double>(*answer_deadline - std::chrono::steady_clock::now()).count() - 0.04;
                seconds = std::max(1e-6, seconds > 0 ? std::min(seconds, room) : room);
            }
            int cut = 0;
            const auto tc = std::chrono::steady_clock::now();
            const int64_t outright = awdec::certain_line(box, mn.sn, memo.side, pmenu, own_i, seconds, &cut);
            last.certain_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count();
            ++last.certain_n; last.certain_cut = std::max(last.certain_cut, cut);
            if (outright >= 0 && !banned.count(mn.prow[size_t(outright)])) pick = mn.prow[size_t(outright)];
        }
    }
    // the seat's unit rule
    if (!pick.empty()) {
        auto it = std::find(rows.begin(), rows.end(), pick);
        int changed = 0;
        void* um = it == rows.end() ? nullptr : awdec::units(box, mn.sn, memo.side, mn.full, int64_t(it - rows.begin()), &changed);
        if (um) {
            const std::string ur = awgt::menu_rows(um)[0];
            if (!banned.count(ur)) { pick = ur; units_menu = um; } else wotr_snapshot_destroy(um);
        }
    }
    return pick;
}

// _apply_copy(st, a, flag): the action applied on a copy (sn: the state's snapshot, with the flag set when one is
// given); its draws from the global Random. Null when the engine refused it.
pyobj::Obj* Bot::apply_copy(const void* sn, const void* menu, int64_t index, const std::string& flag) {
    ++last.applies;
    void* rng = next_apply_rng ? next_apply_rng() : nullptr;
    const bool own = rng == nullptr;
    if (own) { if (!own_rng) { const uint32_t z = 0; own_rng = wotr_random_create_seeded(&z, 1); } rng = own_rng; }
    void* rng_out = nullptr; void* events = nullptr;
    void* after = wotr_snapshot_apply_descriptor_random(sn, ad.obs_tables, menu, rng, memo.side, index, &rng_out, &events);
    if (events) wotr_snapshot_destroy(events);
    if (rng_out) { if (own) { wotr_random_destroy(own_rng); own_rng = rng_out; } else wotr_random_destroy(rng_out); }
    if (!own) wotr_random_destroy(rng);
    if (!after) { last.note = std::string("apply failed: ") + wotr_snapshot_last_error(); return nullptr; }
    const pyobj::Graph g = awgt::export_graph(after);
    wotr_snapshot_destroy(after);
    pyobj::Obj* w = m.decode(g);
    if (!flag.empty()) m.set_attr(w, flag, m.boolean(false));
    return w;
}

std::pair<std::optional<int64_t>, std::string> Bot::search_and_map(pyobj::Obj* st, const Value& dec, const std::string& label,
                                                                   const std::string& flag, std::optional<double> left) {
    using Matched = awad::Adapter::Matched;
    const std::string kind = at(dec, "kind").s;
    if (!flag.empty()) m.set_attr(st, flag, m.boolean(true));
    const auto tm = std::chrono::steady_clock::now();
    auto mn = menu_of(m, st, ad.obs_tables, memo.side);
    last.menu_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - tm).count();
    if (mn->rows.empty()) { last.note = "engine has no action"; return {std::nullopt, "default"}; }
    std::vector<Matched> mts;   // every row matched to an option (map_action), the rest banned
    for (size_t ri = 0; ri < mn->rowsn.size(); ++ri) {
        Matched mt;
        try { mt = ad.map_action(kind, st, dec, row_of(*mn, ri), memo); } catch (...) { mt = Matched(); }
        if (mt.unported) last.unported = "matching " + kind;
        mts.push_back(std::move(mt));
    }
    std::set<std::string> banned;
    std::vector<size_t> mapped;
    for (size_t ri = 0; ri < mts.size(); ++ri) { if (mts[ri].ok) mapped.push_back(ri); else banned.insert(mn->rows[ri]); }
    if (mapped.empty()) { if (!flag.empty()) m.set_attr(st, flag, m.boolean(false)); last.note = "no engine action matches an option"; return {std::nullopt, "default"}; }
    bool single = true;
    for (size_t q : mapped) single &= mts[q].o == mts[mapped[0]].o && mts[q].o_none == mts[mapped[0]].o_none && !mts[q].has_plan && !mts[q].chain;
    if (single) {
        if (!flag.empty()) m.set_attr(st, flag, m.boolean(false));
        last.engine = mn->rows[mapped[0]];
        return {mts[mapped[0]].o_none ? std::nullopt : std::optional<int64_t>(mts[mapped[0]].o), "single"};
    }
    Matched* chosen = nullptr;
    Matched units_mt;
    void* units_menu = nullptr;
    struct Guard { void* p; ~Guard() { if (p) wotr_snapshot_destroy(p); } } g5{nullptr};
    const void* a_menu = mn->full; int64_t a_index = -1;
    std::string how = "single";
    if (mapped.size() == 1) { chosen = &mts[mapped[0]]; a_index = int64_t(mapped[0]); }
    else {
        bool has_sims = false;
        const std::string pick = choose(*mn, banned, label, left, has_sims, units_menu);
        g5.p = units_menu;
        if (has_sims) how = "search";
        if (!pick.empty()) for (size_t q : mapped) if (mn->rows[q] == pick) { chosen = &mts[q]; a_index = int64_t(q); last.engine = pick; break; }
        if (!chosen && !pick.empty() && units_menu && !banned.count(pick)) {   // matched by itself
            const pyobj::Graph ug = awgt::export_graph(units_menu);
            awgt::G gu{ug};
            const auto parts = gu.kids(gu.kids(ug.root)[0]);
            awad::Adapter::Row row; row.die = ug.values[parts[0]].integer; row.type = gu.text(parts[1]);
            row.params = awgt::to_value(gu, parts[2]); row.menu = units_menu; row.index = 0; row.sn = mn->sn;
            try { units_mt = ad.map_action(kind, st, dec, row, memo); } catch (...) { units_mt = Matched(); }
            if (units_mt.ok) { chosen = &units_mt; a_menu = units_menu; a_index = 0; last.engine = pick; last.note = "the seat's unit rule"; }
        }
        if (!chosen) { chosen = &mts[mapped[0]]; a_index = int64_t(mapped[0]); last.engine = mn->rows[mapped[0]]; last.note = "fallback: first matched"; }
    }
    const auto o = chosen->option();
    plan = chosen->plan();
    if (std::getenv("AW_BOT_DEBUG")) std::fprintf(stderr, "[bot] %s: %s -> plan %s, chain %d\n", kind.c_str(), last.engine.substr(0, 200).c_str(), plan ? plan->name.c_str() : "none", int(chosen->chain));
    if (chosen->chain) {   // the engine's chain goes on, on a copy of the state (st under the flag: mn->sn)
        work = apply_copy(mn->sn, a_menu, a_index, flag);
        {   // the action's type
            const pyobj::Graph ag = a_menu == mn->full ? pyobj::Graph() : awgt::export_graph(a_menu);
            const pyobj::Graph& gg = a_menu == mn->full ? mn->mg : ag;
            awgt::G ga{gg};
            const auto rr = ga.kids(gg.root);
            const auto parts = ga.kids(rr[size_t(a_menu == mn->full ? a_index : 0)]);
            work_action = ga.text(parts[1]);
            // the card the engine's action plays (its questions follow)
            work_card.clear();
            if (work_action == "play_event") {
                const Value pv = awgt::to_value(ga, parts[2]);
                const Value& cid = at(pv, "card_id");
                if (cid.kind == Value::Str) { auto it = t.card_out.find(cid.s); if (it != t.card_out.end()) work_card = it->second; }
            }
        }
        work_base = st;
        diff.reset();
    }
    if (!flag.empty()) m.set_attr(st, flag, m.boolean(false));
    return {o, how};
}

// _chain: the next link of the engine's chain, on its copy of the state (none: it has no answer)
std::optional<std::pair<std::optional<int64_t>, std::string>> Bot::chain(const Value& dec, std::optional<double> left) {
    using Matched = awad::Adapter::Matched;
    const std::string kind = at(dec, "kind").s;
    std::unique_ptr<Menu> mn;
    const auto tm = std::chrono::steady_clock::now();
    try { mn = menu_of(m, work, ad.obs_tables, memo.side); } catch (...) { return std::nullopt; }
    last.menu_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - tm).count();
    if (mn->rows.empty() || !chain_fits(work, dec, *mn)) return std::nullopt;
    std::vector<Matched> mts;
    for (size_t ri = 0; ri < mn->rowsn.size(); ++ri) {
        Matched mt;
        try { mt = ad.map_action(kind, work, dec, row_of(*mn, ri), memo); } catch (...) { mt = Matched(); }
        if (mt.unported) last.unported = "matching " + kind;
        mts.push_back(std::move(mt));
    }
    std::set<std::string> banned;
    std::vector<size_t> mapped;
    for (size_t ri = 0; ri < mts.size(); ++ri) { if (mts[ri].ok) mapped.push_back(ri); else banned.insert(mn->rows[ri]); }
    if (mapped.empty()) return std::nullopt;
    last.note = "chain";
    size_t at_i = mapped[0];
    if (mapped.size() > 1) {
        bool has_sims = false; void* units_menu = nullptr;
        const std::string pick = choose(*mn, banned, "action", left, has_sims, units_menu);
        if (units_menu) wotr_snapshot_destroy(units_menu);
        bool found = false;
        if (!pick.empty()) for (size_t q : mapped) if (mn->rows[q] == pick) { at_i = q; found = true; break; }
        if (!found) last.note = "chain, fallback: first matched";
    }
    last.engine = mn->rows[at_i];
    Matched& mt = mts[at_i];
    const auto o = mt.option();
    plan = mt.plan();
    pyobj::Obj* before = work;
    work = apply_copy(mn->sn, mn->full, int64_t(at_i), "");
    if (mt.diff) {   // the link's board diff, as its first answer read it (the questions that follow go on reading it)
        work_base = before;
        diff = *mt.diff;
    }
    return std::make_pair(o, std::string("chain"));
}

bool Bot::chain_fits(pyobj::Obj* w, const Value& dec, const Menu& mn) {
    using M = pyobj::Model;
    const std::string kind = at(dec, "kind").s;
    pyobj::Obj** pco_s = M::attr_slot(w, "pending_card_option");
    pyobj::Obj* pco = (pco_s && (*pco_s)->tag != WOTR_NONE) ? *pco_s : nullptr;
    auto card_name = [&]() -> std::string { if (!pco || pco->tag != WOTR_DICT) return "\x01"; pyobj::Obj* n = M::dict_get(pco, "card_name"); return n && n->tag == WOTR_STRING ? n->s : "\x01"; };
    if (kind == "ARMY_MOVE")
        return !is_none(w, "pending_army_move_die") || !is_none(w, "pending_card_followup") ||
               (pco && card_name() != "__companion_moves__" && card_name() != "__nazgul_moves__");
    if (kind == "RECRUIT") return !is_none(w, "pending_muster_die") || pco;
    if (kind == "MOVE_COMPANIONS") return pco && card_name() == "__companion_moves__";
    if (kind == "MOVE_MINIONS") return pco && card_name() == "__nazgul_moves__";
    if (kind == "CARD_CHOICE") return pco || !is_none(w, "pending_card_followup");
    for (auto rn : mn.rowsn) { const std::string ty = row_type(mn.mg, rn); if (ty == "card_option" || ty == "army_move" || ty == "army_attack") return true; }
    return false;
}

bool Bot::chain_over(pyobj::Obj* w, const std::string& kind) {
    using M = pyobj::Model;
    pyobj::Obj** pco_s = M::attr_slot(w, "pending_card_option");
    pyobj::Obj* pco = (pco_s && (*pco_s)->tag != WOTR_NONE) ? *pco_s : nullptr;
    auto card_name = [&]() -> std::string { if (!pco || pco->tag != WOTR_DICT) return "\x01"; pyobj::Obj* n = M::dict_get(pco, "card_name"); return n && n->tag == WOTR_STRING ? n->s : "\x01"; };
    if (kind == "ARMY_MOVE") return is_none(w, "pending_army_move_die") && is_none(w, "pending_card_followup");
    if (kind == "RECRUIT") return is_none(w, "pending_muster_die") && !pco;
    if (kind == "MOVE_COMPANIONS") return !pco || card_name() != "__companion_moves__";
    if (kind == "MOVE_MINIONS") return !pco || card_name() != "__nazgul_moves__";
    return false;
}

// diff_answer: a card question the engine settled inside its action, read off its board after the action
std::optional<int64_t> Bot::diff_answer(const Value& dec) {
    const std::string kind = at(dec, "kind").s;
    if (!work || !work_base || (kind != "CARD_CHOICE" && kind != "ARMY_SELECTION" && kind != "ARMY_MOVE" && kind != "SEPARATE_COMPANIONS"))
        return std::nullopt;
    if (!diff) diff = ad.board_diff(work_base, work);
    return ad.diff_pick(dec, *diff, {work, work_card, work_action});
}

namespace {
// played_decision(ev): a choice event (a decision the referee made for the bot) as a decision with that one option
Value played_decision(const Value& ev) {
    Value o = at(ev, "option");
    if (o.kind != Value::Obj) { o = Value(); o.kind = Value::Obj; }
    bool has = false;
    for (auto& kv : o.obj) if (kv.first == "o") { kv.second = at(ev, "o").is_null() ? I(0) : at(ev, "o"); has = true; }
    if (!has) o.obj.push_back({"o", at(ev, "o").is_null() ? I(0) : at(ev, "o")});
    Value opts_; opts_.kind = Value::Arr; opts_.arr.push_back(o);
    Value ctx; ctx.kind = Value::Obj;
    Value d; d.kind = Value::Obj;
    d.obj.push_back({"kind", at(ev, "decision")});
    d.obj.push_back({"side", at(ev, "side")});
    d.obj.push_back({"options", opts_});
    d.obj.push_back({"context", ctx});
    return d;
}
}  // namespace

// _reenter: the engine's action the referee began for the bot (a scenario's played_for_you): the engine's actions at
// the start position that the played ACTION matches (the search picks among them), then each of the bot's later
// played answers followed on its copy. Only when the referee's first choice was the bot's ACTION and no other ACTION
// followed.
void Bot::reenter(const std::vector<Value>& pre, std::optional<double> left) {
    const std::string mine = memo.side ? "SHADOW" : "FREE_PEOPLES";
    if (pre.empty() || !start_view) return;
    auto str_of = [](const Value& v, const char* k) { const Value& x = at(v, k); return x.kind == Value::Str ? x.s : std::string(); };
    if (str_of(pre[0], "side") != mine || str_of(pre[0], "decision") != "ACTION") return;
    for (size_t i = 1; i < pre.size(); ++i) if (str_of(pre[i], "decision") == "ACTION") return;
    pyobj::Obj* st = ad.build_state(*start_view, memo);
    const Value fdec = played_decision(pre[0]);
    const auto su = ad.setup("ACTION", st, fdec, *start_view, memo);
    if (!su.ported || su.label.empty()) return;
    const auto r = search_and_map(st, fdec, su.label, su.flag, left);
    if (!r.first) {
        if (report) std::fprintf(stderr, "[aw]   %s: the played ACTION matches no engine action (%s)\n", game.c_str(), last.note.c_str());
        plan.reset(); drop_work();
        return;
    }
    for (size_t i = 1; i < pre.size(); ++i) if (str_of(pre[i], "side") == mine) follow(played_decision(pre[i]), left);
}

// _follow: an answer the referee gave for the bot, followed by its plan or its engine's chain
void Bot::follow(const Value& dec, std::optional<double> left) {
    const std::string kind = at(dec, "kind").s;
    if (plan && plan->takes(kind)) { if (plan->answer(dec)) return; }
    plan.reset();
    if (work && CHAIN_KINDS.count(kind)) {
        if (chain(dec, left)) return;
        if (diff_answer(dec)) return;
        if (report) std::fprintf(stderr, "[aw]   %s: the played %s is not the engine's\n", game.c_str(), kind.c_str());
        return;
    }
    if (!CHAIN_KINDS.count(kind)) drop_work();
}

}  // namespace awbot
