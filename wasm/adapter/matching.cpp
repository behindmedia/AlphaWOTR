// The option matching: protocol/wotr_protocol_bot.py map_action and the MAPPERS (the adapter's lines 2272-2936),
// for the questions the adapter searches. An engine row (die, action type, parameters) of the box's menu is matched
// to the referee's option; the result says the option, whether a plan follows (Match.has_plan) and whether the
// engine's chain goes on (Match.chain). Not ported yet: m_advance and the move / attack answers of m_card_choice
// (their plans apply the action on a copy of the state first): they return `unported`.
#include "adapter.h"
#include "opts.h"
#include "plans.h"
#include "wotr_random.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <set>

using pyobj::Obj;
using awjson::Value;

namespace awad {
namespace {
using namespace awad::opt;
std::string plain(const std::string& x) { std::string o; for (unsigned char c : x) if (std::isalnum(c) && c < 128) o += char(std::tolower(c)); return o; }
std::string lower(std::string x) { for (char& c : x) c = char(std::tolower(static_cast<unsigned char>(c))); return x; }
const std::map<std::string, std::vector<std::string>> DIE_ACTIONS = {
    {"draw_card", {"draw_card"}}, {"play_event", {"play_card"}}, {"army_attack", {"attack", "attack_with_leader"}},
    {"army_move", {"move_armies", "move_army_with_leader"}}, {"advance_political", {"diplomacy"}},
    {"muster_recruit", {"recruit"}}, {"muster_elite", {"recruit"}}, {"muster_minion", {"play_minion"}},
    {"transform_gandalf", {"play_companion"}}, {"crown_aragorn", {"play_companion"}},
    {"fellowship_move", {"move_fellowship"}}, {"fellowship_hide", {"hide_fellowship", "hide_fellowship_strider"}},
    {"companion_move", {"move_companions"}}, {"companion_separate", {"separate_companions"}},
    {"nazgul_move", {"move_minions"}}, {"mouth_move", {"move_minions"}}, {"discard_table_card", {"force_discard"}}};
}  // namespace

std::string Adapter::slug(const std::string& key) const {
    const std::string san = awad_sanitize(key);
    for (const auto& kv : t.region_in) if (kv.second == san) return kv.first;
    return "";
}

Adapter::Matched Adapter::map_action(const std::string& kind, Obj* st, const Value& dec, const Row& a, const Memo& memo) {
    using M = pyobj::Model;
    Matched r;
    const Value& p = a.params;
    auto pstr = [&](const char* k) -> std::string { return pstr_of(p, k); };
    auto pslug = [&](const char* k) { const std::string s = pstr(k); return s.empty() ? std::string() : slug(s); };
    auto first_of = [&](const std::vector<const Value*>& c) { if (!c.empty() && has_o(c[0])) { r.ok = true; r.o = at(*c[0], "o").as_int(); } };
    // _m(cand, plan, chain)
    auto mm = [&](const std::vector<const Value*>& c, bool plan = false, bool chain = false) {
        if (c.empty()) return r;
        r.ok = true; r.o_none = !has_o(c[0]); if (!r.o_none) r.o = at(*c[0], "o").as_int();
        r.has_plan = plan; r.chain = chain; return r;
    };
    auto die_options = [&](const std::vector<std::string>& actions, const std::vector<F>& fields = {}) {
        std::vector<const Value*> c;
        for (const auto& name : actions) {
            std::vector<F> fl = {f("die", I(a.die)), f("action", S(name))};
            fl.insert(fl.end(), fields.begin(), fields.end());
            for (auto* o : opts(dec, "use_die", fl)) c.push_back(o);
        }
        return c;
    };
    auto char_out = [&](const std::string& name) -> std::string { for (const auto& kv : t.char_in) if (kv.second == name) return kv.first; return ""; };
    auto card_key = [&](const std::string& cid) -> std::string { auto it = t.card_out.find(cid); return it == t.card_out.end() ? "" : it->second; };
    const std::string& ty = a.type;

    if (kind == "ACTION") {                                   // m_action
        if (ty == "pass") { first_of(opts(dec, "pass")); return r; }
        if (ty == "elven_ring") {
            const std::string face = t.face_by_value.count(pstr("new_face")) ? t.face_by_value.at(pstr("new_face")) : "";
            std::string to;
            for (const auto& kv : t.face_in) if (kv.second == face) to = kv.first;
            first_of(opts(dec, "elven_ring", {f("die", at(p, "target_die_idx")), fstr("to", to, !to.empty())}));
            return r;
        }
        auto da = DIE_ACTIONS.find(ty);
        if (da == DIE_ACTIONS.end()) return r;
        std::vector<std::string> acts = da->second;
        if (ty == "draw_card") return mm(die_options(acts, {f("deck", at(p, "deck"))}));
        if (ty == "play_event") { const std::string k = card_key(pstr("card_id")); return mm(die_options(acts, {fstr("card", k, !k.empty())}), false, true); }
        if (ty == "advance_political") {
            auto ni = t.nation_out.find(pstr("nation"));
            const F nat = ni == t.nation_out.end() ? fnone("nation") : f("nation", S(ni->second));
            std::vector<const Value*> c;
            if (at(p, "companion_ability").truthy()) {
                const std::string ck = char_out(pstr("companion_ability"));
                c = die_options({"companion_politics"}, {fstr("character", ck, !ck.empty())});
                if (c.empty()) c = die_options({"diplomacy"}, {nat});
            } else c = die_options({"diplomacy"}, {nat});
            return mm(c);
        }
        if (ty == "muster_minion") {
            const std::string ck = char_out(pstr("minion")), rg = pslug("region");
            return mm(die_options(acts, {fstr("character", ck, !ck.empty()), fstr("region", rg, !rg.empty())}));
        }
        if (ty == "transform_gandalf") { const std::string rg = pslug("region"); return mm(die_options(acts, {f("character", S("gandalf-the-white")), fstr("region", rg, !rg.empty())})); }
        if (ty == "crown_aragorn") return mm(die_options(acts, {f("character", S("aragorn"))}));
        if (ty == "fellowship_move" || ty == "fellowship_hide") {
            if (ty == "fellowship_hide" && at(p, "strider_ability").truthy()) acts = {"hide_fellowship_strider", "hide_fellowship"};
            return mm(die_options(acts));
        }
        if (ty == "discard_table_card") {
            const std::string k = card_key(pstr("card_id"));
            const auto cand = die_options(acts, {fstr("card", k, !k.empty())});
            const bool ring = pstr("payment") == "ring";
            std::vector<const Value*> pick;
            for (auto* o : cand) if (at(*o, "elven_ring").truthy() == ring) pick.push_back(o);
            return mm(pick.empty() ? cand : pick);
        }
        // the die's face: st.dice[me][die_idx].current
        std::string face;
        Obj* dice = M::dict_get(M::attr(st, "dice"), this->m.enumv("game_state.Side", memo.side ? "SHADOW" : "FREE_PEOPLES"));
        if (dice && a.die >= 0 && size_t(a.die) < dice->k.size()) {
            Obj* cur = M::attr(dice->k[size_t(a.die)], "current");
            if (cur->tag == WOTR_ENUM) face = cur->k[1]->s;
        }
        auto ordered = [&]() {
            std::vector<std::string> order = acts;
            if (face == "CHARACTER") std::reverse(order.begin(), order.end());
            auto c = die_options({order[0]});
            if (c.empty()) c = die_options(std::vector<std::string>(order.begin() + 1, order.end()));
            return c;
        };
        if (ty == "army_move") {   // the plan: a function (army_move_plan, made for the action picked)
            const auto c = ordered(); if (c.empty()) return r;
            Matched x = mm(c, true, true);
            const Memo mc = memo;
            x.make_plan = [this, st, a, mc]() { return army_move_plan_obj(st, a, mc, false); };
            return x;
        }
        if (ty == "army_attack") {
            const auto c = ordered(); if (c.empty()) return r;
            Matched x = mm(c, true, false);
            const Value fr = slugv(at(p, "from")), to = slugv(at(p, "to"));
            x.make_plan = [fr, to]() { return std::make_shared<AttackPlan>(fr, to); };
            return x;
        }
        if (ty == "muster_recruit" || ty == "muster_elite" || ty == "companion_move" || ty == "nazgul_move" || ty == "mouth_move") {
            const auto c = die_options(acts); if (c.empty()) return r;
            Matched x = mm(c, true, true);
            x.make_plan = plan_maker(st, a, memo);
            return x;
        }
        if (ty == "companion_separate") {
            const auto c = die_options(acts); if (c.empty()) return r;
            Matched x = mm(c, true, false);
            std::vector<std::string> who;
            const Value& cs = at(p, "companions");
            if (cs.truthy()) { for (const Value& n : cs.arr) if (n.kind == Value::Str) who.push_back(n.s); }
            else if (at(p, "companion").kind == Value::Str) who.push_back(at(p, "companion").s);
            std::vector<std::string> keys;
            for (const auto& n : who) { const std::string k = char_out(n); if (!k.empty()) keys.push_back(k); }
            const Value to = slugv(at(p, "to_region"));
            x.make_plan = [keys, to]() { return std::make_shared<SeparatePlan>(keys, to); };
            return x;
        }
        return r;
    }
    if (kind == "FELLOWSHIP_PHASE") {
        if (ty == "fellowship_declare") { const std::string rg = pslug("region"); return mm(opts(dec, "declare", {fstr("region", rg, !rg.empty())})); }
        if (ty == "pass") return mm(opts(dec, "done"));
        return r;
    }
    if (kind == "HUNT_ALLOCATION") {
        if (ty == "eye_allocate") return mm(opts(dec, "hunt_dice", {f("count", I(at(p, "count").kind == Value::Null ? 0 : at(p, "count").as_int()))}));
        return r;
    }
    if (kind == "DISCARD_EXCESS_CARD") {
        if (ty == "discard_card") { const std::string k = card_key(pstr("card_id")); return mm(opts(dec, "discard", {fstr("card", k, !k.empty())})); }
        return r;
    }
    if (kind == "COMBAT_CARD") {
        if (ty == "combat_card_play") { const std::string k = card_key(pstr("card_id")); return mm(opts(dec, "combat_card", {fstr("card", k, !k.empty())})); }
        if (ty == "combat_card_none") return mm(opts(dec, "none"));
        return r;
    }
    if (kind == "CASUALTY") {                                 // m_casualty: CasualtyPlan + casualty_pick
        if (ty != "casualty_allocate") return r;
        const int64_t k = at(p, "count").kind == Value::Null ? 0 : at(p, "count").as_int();
        const auto red = opts(dec, "casualty", {f("unit", S("elite")), f("effect", S("reduce"))});
        const auto rem_r = opts(dec, "casualty", {f("unit", S("regular"))});
        const auto rem_e = opts(dec, "casualty", {f("unit", S("elite")), f("effect", S("remove"))});
        const Value& ctx = or_empty(dec, "context");
        const int64_t hits = ctx.get("hits") ? at(ctx, "hits").as_int() : 1;
        const Value* pick = nullptr;
        if (k > 0 && !red.empty()) pick = red[0];
        else if (!rem_r.empty()) pick = rem_r[0];
        else if (!rem_e.empty() && hits >= 2) pick = rem_e[0];
        else if (!red.empty()) pick = red[0];
        else if (!rem_e.empty()) pick = rem_e[0];
        if (!has_o(pick)) return r;
        r.ok = true; r.o = at(*pick, "o").as_int(); r.has_plan = true;
        const Value dc = dec;
        r.make_plan = [dc, k]() { auto pl = std::make_shared<CasualtyPlan>(casualty_key(dc), k); casualty_pick(dc, k > 0, pl.get()); return std::shared_ptr<Plan>(pl); };
        return r;
    }
    if (kind == "CEASE_ATTACK") {
        if (ty == "combat_continue" || ty == "siege_press") return mm(opts(dec, "continue"));
        if (ty == "combat_cease" || ty == "siege_no_press") return mm(opts(dec, "cease"));
        return r;
    }
    if (kind == "EXTEND_SIEGE") {
        if (ty == "siege_press") return mm(opts(dec, "extend"));
        if (ty == "siege_no_press") return mm(opts(dec, "stop"));
        return r;
    }
    if (kind == "RETREAT") {
        if (ty == "combat_retreat") { const std::string rg = pslug("to_region"); return mm(opts(dec, "retreat", {fstr("region", rg, !rg.empty())})); }
        if (ty == "combat_no_retreat") return mm(opts(dec, "stand"));
        return r;
    }
    if (kind == "FIELD_OR_SIEGE") {
        if (ty == "retreat_into_siege") return mm(opts(dec, "siege"));
        if (ty == "combat_no_retreat" || ty == "combat_retreat") {
            Matched x = mm(opts(dec, "field"), true);
            std::optional<Value> to;
            if (ty == "combat_retreat") to = slugv(at(p, "to_region"));
            x.make_plan = [to]() { return std::make_shared<RetreatPlan>(to); };
            return x;
        }
        return r;
    }
    if (kind == "WHITE_RIDER") {
        if (ty == "shine") return mm(opts(dec, "use"));
        if (ty == "shine_none") return mm(opts(dec, "skip"));
        return r;
    }
    if (kind == "HUNT_CASUALTY") {
        if (ty == "hunt_take_corruption") return mm(opts(dec, "corruption"));
        if (ty == "hunt_sacrifice_guide") return mm(opts(dec, "guide"));
        if (ty == "hunt_sacrifice_random") return mm(opts(dec, "random"));
        return r;
    }
    if (kind == "GUIDE_ABILITY") {
        if (ty == "hunt_gollum_reveal" || ty == "hunt_guide_separate") return mm(opts(dec, "use"));
        if (ty == "hunt_gollum_no_reveal") return mm(opts(dec, "skip"));
        if (ty == "hunt_take_corruption" || ty == "hunt_sacrifice_guide" || ty == "hunt_sacrifice_random") {
            Matched x = mm(opts(dec, "skip"), true);
            const std::string okind = ty == "hunt_take_corruption" ? "corruption" : ty == "hunt_sacrifice_guide" ? "guide" : "random";
            x.make_plan = [okind]() { return std::make_shared<AnswerPlan>("HUNT_CASUALTY", okind); };
            return x;
        }
        return r;
    }
    if (kind == "REVEAL_FELLOWSHIP") {
        if (ty == "fellowship_reveal_place") { const std::string rg = pslug("region"); return mm(opts(dec, "reveal", {fstr("region", rg, !rg.empty())})); }
        return r;
    }
    if (kind == "CHOOSE_GUIDE") {
        if (ty == "choose_guide") { const std::string ck = char_out(pstr("companion")); return mm(opts(dec, "guide", {fstr("character", ck, !ck.empty())})); }
        return r;
    }
    if (kind == "ADVANCE") {
        if (ty == "victor_stay") return mm(opts(dec, "stay"));
        if (ty != "victor_advance") return r;
        // m_advance: ArmyMovePlan(advance=True).answer(dec). The plan applies the row on a copy; done here for the
        // row the search picks only (Match.option / plan): an advance's menu lists every split of the Army (1,417
        // rows seen), and the box checks each applied row against the legal menu it works out again, so applying
        // every row took seconds (a timeout on the module, 2026-11-23). A row of the box's own menu applies and moves
        // something, so each is matched, as in the Python.
        r.ok = true; r.o_none = true; r.has_plan = true;
        const Memo mc = memo;
        r.make_plan = [this, st, a, mc]() { return army_move_plan_obj(st, a, mc, true); };
        r.resolve = [dec](Plan* pl) -> std::optional<int64_t> { return pl ? pl->answer(dec) : std::nullopt; };
        return r;
    }
    if (kind == "CARD_CHOICE") {                              // m_card_choice
        if (ty == "card_option") {
            const Value& ch = or_empty(p, "choices");
            if (ch.get("palantir_deck")) return mm(opts(dec, "draw", {f("deck", at(ch, "palantir_deck"))}));
            if (ch.get("eliminate")) {   // Stormcrow's piece (the Free Peoples' choice)
                const Value& e = or_empty(ch, "eliminate");
                std::vector<const Value*> c;
                for (auto* o : opts(dec, "card_choice")) {
                    const Value& u = at(*o, "unit");
                    const Value rs = slugv(at(e, "region"));
                    if (lower(u.kind == Value::Str ? u.s : "") == (at(e, "kind").kind == Value::Str ? at(e, "kind").s : "\x01") &&
                        equal(at(*o, "region"), rs) && at(*o, "region").kind == rs.kind) c.push_back(o);
                }
                return mm(c);
            }
            if (ch.get("ents_chain_play")) {   // (a chain: the card played next asks its own questions)
                std::vector<const Value*> c;
                for (auto* o : opts(dec, "card_choice")) {
                    if (at(*o, "skip").truthy()) continue;
                    const Value& tc = at(*o, "target_card");
                    const std::string cid = tc.kind == Value::Str ? t.card(tc.s) : "";
                    if (!cid.empty() && cid == pstr_of(ch, "ents_chain_play")) c.push_back(o);
                }
                return mm(c, false, true);
            }
            if (at(ch, "_ents_chain_done").truthy()) return mm(opts(dec, "card_choice", {f("skip", B(true))}));
            if (ch.get("use_hunt_reduction")) {
                if (at(ch, "use_hunt_reduction").truthy()) {
                    std::vector<const Value*> c;
                    for (auto* o : opts(dec, "card_choice")) {
                        const Value& tc = at(*o, "target_card");
                        const std::string cid = tc.kind == Value::Str ? t.card(tc.s) : "";
                        if (!cid.empty() ? cid == pstr_of(ch, "card_id") : at(ch, "card_id").kind == Value::Null) c.push_back(o);
                    }
                    return mm(c);
                }
                return mm(opts(dec, "card_choice", {f("skip", B(true))}));
            }
            if (at(ch, "_comp_done").truthy() || at(ch, "_naz_done").truthy()) {
                auto e = opts(dec, "end");
                const Value* o = (!e.empty() && has_o(e[0])) ? e[0] : nullptr;
                if (!o) { auto s = opts(dec, "card_choice", {f("skip", B(true))}); if (!s.empty() && has_o(s[0])) o = s[0]; }
                if (!o) return r;
                r.ok = true; r.o = at(*o, "o").as_int(); r.chain = true; return r;
            }
            if (ch.get("companion_action") && !at(ch, "companion_action").is_null()) return companion_action_match(dec, ch);
            if (ch.get("recruit_counts")) {
                // a recruitment card's allocation (when the Regulars run short): answered off the board this option
                // makes (A New Power is Rising adds its Orthanc Elites by itself)
                Obj* w = apply_row(st, a, memo.side);
                if (!w) return r;
                auto d = std::make_shared<Diff>(board_diff(st, w));
                const auto o = diff_pick(dec, *d, PickCtx{});
                if (!o) return r;
                r.ok = true; r.o = *o; r.chain = true; r.diff = d;
                return r;
            }
            if (ch.get("dice_faces")) {   // The Lidless Eye: the faces of the dice turned to Eyes
                std::vector<std::string> faces;
                for (const Value& x : at(ch, "dice_faces").arr) if (x.kind == Value::Str) faces.push_back(x.s.rfind("\x01" "enum:", 0) == 0 ? x.s.substr(6) : x.s);
                std::map<int64_t, std::string> face_of;
                Obj* dice = M::attr(st, "dice");
                Obj* mine = dice && dice->tag == WOTR_DICT ? M::dict_get(dice, side_enum(memo.side)) : nullptr;
                if (mine) for (size_t i = 0; i < mine->k.size(); ++i) {
                    Obj* d = mine->k[i];
                    Obj** used = M::attr_slot(d, "used");
                    Obj** cur = M::attr_slot(d, "current");
                    if ((used && M::truthy(*used)) || !cur || (*cur)->tag != WOTR_ENUM) continue;
                    face_of[int64_t(i)] = (*cur)->k[1]->s;
                }
                auto plan = std::make_shared<EyeDicePlan>(faces, card_key(pstr("card_id")), face_of);
                const auto o = plan->answer(dec);
                if (!o) return r;
                r.ok = true; r.o = *o; r.has_plan = true; r.chain = true;
                r.make_plan = [plan]() { return std::shared_ptr<Plan>(plan); };
                return r;
            }
            if (ch.get("unit_combo") && !at(ch, "unit_combo").is_null()) {   // Cirdan's Ships (line 3372; wotr #1431)
                auto plan = std::make_shared<UnitComboPlan>(card_key(pstr("card_id")), slugv(at(ch, "region")), pstr_of(ch, "unit_combo"));
                const auto o = plan->answer(dec);
                if (!o) return r;
                r.ok = true; r.o = *o; r.has_plan = true; r.chain = true;
                r.make_plan = [plan]() { return std::shared_ptr<Plan>(plan); };
                return r;
            }
            if (ch.get("rage_split") && at(ch, "rage_split").kind == Value::Obj) {
                auto plan = std::make_shared<RageSplitPlan>(at(ch, "rage_split"));
                const auto o = plan->answer(dec);
                if (!o) return r;
                r.ok = true; r.o = *o; r.has_plan = true; r.chain = true;
                r.make_plan = [plan]() { return std::shared_ptr<Plan>(plan); };
                return r;
            }
            std::set<std::pair<std::string, std::string>> toks;
            choice_tokens(ch, toks);
            const int64_t best = score_options(dec, toks, !ch.truthy());
            if (best < 0) return r;
            r.ok = true; r.o = best; r.chain = true; return r;
        }
        if (ty == "followup_done") {
            auto s = opts(dec, "card_choice", {f("skip", B(true))});
            const Value* o = (!s.empty() && has_o(s[0])) ? s[0] : nullptr;
            if (!o) { auto e = opts(dec, "end"); if (!e.empty() && has_o(e[0])) o = e[0]; }
            if (!o) return r;
            r.ok = true; r.o = at(*o, "o").as_int(); r.chain = true; return r;
        }
        if (ty == "combat_effect_choice") {
            // (Words of Power's Companion: the printed name; m_card_choice, line 3390; wotr #1431)
            if (p.get("target") && !at(p, "target").is_null()) { const std::string ck = char_out(pstr("target")); return mm(opts(dec, "card_choice", {fstr("character", ck, !ck.empty())})); }
            return mm(opts(dec, "card_choice", {f("count", I(at(p, "count").kind == Value::Null ? 0 : at(p, "count").as_int()))}));
        }
        if (ty == "combat_retreat") { const std::string rg = pslug("to_region"); return mm(opts(dec, "card_choice", {fstr("region", rg, !rg.empty())})); }
        if (ty == "army_move" || ty == "army_attack") {
            const std::string fr = pslug("from"), to = pslug("to");
            auto c = opts(dec, "card_choice", {fstr("from", fr, !fr.empty()), fstr("to", to, !to.empty())});
            if (!c.empty()) { r.ok = true; r.o_none = !has_o(c[0]); if (!r.o_none) r.o = at(*c[0], "o").as_int(); r.chain = true; return r; }
            const std::string word = ty == "army_move" ? "move" : "attack";
            for (auto* o : opts(dec, "card_choice")) if (lower(at(*o, "answer").kind == Value::Str ? at(*o, "answer").s : "") == word) {
                if (ty == "army_move") {   // Match(o, army_move_plan(...), chain=True): a plan unless the move fails
                    auto plan = army_move_plan_obj(st, a, memo, false);
                    r.ok = true; r.o = at(*o, "o").as_int(); r.has_plan = bool(plan); r.chain = true;
                    if (plan) r.make_plan = [plan]() { return plan; };
                    return r;
                }
                r.ok = true; r.o = at(*o, "o").as_int(); r.has_plan = true; r.chain = true;
                const Value frv = fr.empty() ? Value() : S(fr), tov = to.empty() ? Value() : S(to);
                r.make_plan = [frv, tov]() { return std::make_shared<AttackPlan>(frv, tov); };
                return r;
            }
            return r;
        }
        return r;
    }
    // the mappers only an engine chain reaches (m_army_move, m_recruit, m_move_companions, m_move_minions)
    auto sl = [&](const Value& v) { return slugv(v); };
    auto fv = [](const char* k, const Value& v) { return v.is_null() ? fnone(k) : f(k, v); };
    const Value& ch = or_empty(p, "choices");
    if (kind == "ARMY_MOVE") {
        if (ty == "pass" || ty == "followup_done") return mm(opts(dec, "end"));
        if (ty == "card_option") {
            if (at(ch, "_done").truthy() || !ch.truthy()) return mm(opts(dec, "end"), false, true);
            const Value fr = sl(at(ch, "from_region").truthy() ? at(ch, "from_region") : at(ch, "from"));
            const Value to = sl(at(ch, "to_region").truthy() ? at(ch, "to_region") : at(ch, "to"));
            if (fr.is_null() || to.is_null()) return r;
            const auto cand = opts(dec, "army_move", {f("from", fr), f("to", to)});
            std::vector<const Value*> all;
            for (auto* o : cand) if (at(*o, "all").truthy()) all.push_back(o);
            return mm(all.empty() ? cand : all, false, true);
        }
        if (ty != "army_move") return r;
        const auto cand = opts(dec, "army_move", {fv("from", sl(at(p, "from"))), fv("to", sl(at(p, "to")))});
        if (cand.empty()) return r;
        Matched x = mm(cand, true, true);
        const Memo mc = memo;
        x.make_plan = [this, st, a, mc]() { auto pl = army_move_plan_obj(st, a, mc, false); if (pl) static_cast<ArmyMovePlan*>(pl.get())->started = true; return pl; };
        std::vector<int64_t> os; std::vector<bool> alls;
        for (auto* o : cand) { os.push_back(has_o(o) ? at(*o, "o").as_int() : -1); alls.push_back(at(*o, "all").truthy()); }
        x.resolve = [os, alls](Plan* pl) -> std::optional<int64_t> {
            if (!pl) return std::nullopt;
            const bool every = static_cast<ArmyMovePlan*>(pl)->everything;
            for (size_t i = 0; i < os.size(); ++i) if (alls[i] == every) return os[i];
            return os[0];
        };
        return x;
    }
    if (kind == "RECRUIT") {
        if (ty == "pass") return mm(opts(dec, "end"));
        if (ty != "muster_recruit" && ty != "muster_elite") return r;
        auto pl = plan_maker(st, a, memo)();
        auto* rp = static_cast<RecruitPlan*>(pl.get());
        return mm(opts(dec, "recruit", {fv("region", rp->region), fv("nation", rp->nation), fv("unit", rp->unit)}), false, true);
    }
    if (kind == "MOVE_COMPANIONS") {
        if (ty != "card_option") return r;
        if (at(ch, "_comp_done").truthy()) return mm(opts(dec, "end"));
        if (at(ch, "from_region").truthy() && at(ch, "companions").truthy()) {
            std::vector<std::string> keys;
            for (const Value& n : at(ch, "companions").arr) if (n.kind == Value::Str) { const std::string k = char_out(n.s); if (!k.empty()) keys.push_back(k); }
            auto pl = std::make_shared<CompanionMovePlan>(keys, sl(at(ch, "from_region")), sl(at(ch, "to_region")));
            const auto o = pl->answer(dec);
            if (!o) return r;
            r.ok = true; r.o = *o; r.has_plan = true; r.chain = true;
            r.make_plan = [pl]() { return std::shared_ptr<Plan>(pl); };
            return r;
        }
        return r;
    }
    if (kind == "MOVE_MINIONS") {
        std::shared_ptr<Plan> pl;
        if (ty == "card_option") {
            if (at(ch, "_naz_done").truthy()) return mm(opts(dec, "end"));
            const Value fr = sl(at(ch, "from_region")), to = sl(at(ch, "to_region"));
            if (fr.is_null() || to.is_null()) return r;
            pl = std::make_shared<MinionPlan>(t, std::vector<std::tuple<std::string, Value, Value>>{{at(ch, "include_wk").truthy() ? "witch-king" : "nazgul", fr, to}});
        } else if (ty == "nazgul_move" || ty == "mouth_move") pl = plan_maker(st, a, memo)();
        else return r;
        const auto o = pl->answer(dec);
        if (!o) return r;
        r.ok = true; r.o = *o; r.has_plan = true; r.chain = true;
        r.make_plan = [pl]() { return pl; };
        return r;
    }
    if (kind == "SEPARATE_COMPANIONS") {   // m_separate: a card's separation (I Will Go Alone, The Breaking of the Fellowship)
        if (ty != "card_option") return r;
        std::vector<std::string> comps;
        for (const Value& n : at(ch, "companions").arr) { const std::string k = n.kind == Value::Str ? char_out(n.s) : ""; if (k.empty()) return r; comps.push_back(k); }
        const bool removed = at(ch, "destination").kind == Value::Str && at(ch, "destination").s == "__remove_from_play__";
        const Value dest = removed ? Value() : slugv(at(ch, "destination"));
        if (comps.empty() || (!removed && dest.is_null())) return r;
        auto plan = std::make_shared<SeparatePlan>(comps, dest, removed);   // the plan of this action only
        const auto o = plan->answer(dec);
        if (!o) return r;
        r.ok = true; r.o = *o; r.has_plan = true; r.chain = true;
        r.make_plan = [plan]() { return std::shared_ptr<Plan>(plan); };
        return r;
    }
    static const std::set<std::string> NO_MAPPER = {"ARMY_SELECTION", "JOIN_GROUP", "CHOOSE_ATTACK", "SEPARATE_COMPANIONS", "REMOVE_EXCESS_UNIT"};
    r.unported = !(kind == "ACTION") && !NO_MAPPER.count(kind);
    return r;
}

// companion_action_match: Gwaihir the Windlord, We Prove Swifter ({'companion_action': 'sep_<name>' or 'group' +
// 'companions', or 'move_<name>' or 'move_group' + 'companions'; 'destination'}): "Separate", or the Companion moving
// first, then the Companions one by one and the destination
Adapter::Matched Adapter::companion_action_match(const Value& dec, const Value& ch) {
    Matched r;
    const std::string ca = at(ch, "companion_action").kind == Value::Str ? at(ch, "companion_action").s : "";
    const Value dest = slugv(at(ch, "destination"));
    if (dest.is_null()) return r;
    std::vector<std::string> who;
    if (ca == "group" || ca == "move_group") { for (const Value& n : at(ch, "companions").arr) who.push_back(n.kind == Value::Str ? n.s : ""); }
    else if (ca.rfind("sep_", 0) == 0 || ca.rfind("move_", 0) == 0) who.push_back(ca.substr(ca.find('_') + 1));
    else return r;
    std::vector<std::string> keys;
    for (const auto& n : who) { const std::string k = char_out(n); if (k.empty()) return r; keys.push_back(k); }
    if (keys.empty()) return r;
    if (ca == "group" || ca.rfind("sep_", 0) == 0) {
        const Value* o = nullptr;
        for (auto* x : opts(dec, "card_choice")) if (lower(at(*x, "answer").kind == Value::Str ? at(*x, "answer").s : "") == "separate") { o = x; break; }
        if (!has_o(o)) return r;
        r.ok = true; r.o = at(*o, "o").as_int(); r.has_plan = true; r.chain = true;
        auto plan = std::make_shared<SeparatePlan>(keys, dest);
        r.make_plan = [plan]() { return std::shared_ptr<Plan>(plan); };
        return r;
    }
    std::vector<const Value*> cand;
    for (auto* o : opts(dec, "move_companion")) {
        const Value& c = at(*o, "character");
        if (c.kind == Value::Str && std::find(keys.begin(), keys.end(), c.s) != keys.end() && equal(at(*o, "to"), dest) && at(*o, "to").kind == dest.kind) cand.push_back(o);
    }
    if (cand.empty() || !has_o(cand[0])) return r;
    auto plan = std::make_shared<CompanionMovePlan>(keys, at(*cand[0], "from"), dest);
    plan->started = true;
    r.ok = true; r.o = at(*cand[0], "o").as_int(); r.has_plan = true; r.chain = true;
    r.make_plan = [plan]() { return std::shared_ptr<Plan>(plan); };
    return r;
}

// a parameter's text (an Enum's member name)
// figures(st, key, side): {(nation, unit): n} and the Character keys of `side` in a region
Adapter::Figures Adapter::figures(Obj* st, const std::string& key, int side) {
    using M = pyobj::Model;
    Figures out;
    Obj* a = army_at(st, key);
    if (!a) return out;
    static const std::set<std::string> FP = {"ELVES", "DWARVES", "GONDOR", "THE_NORTH", "ROHAN"};
    for (Obj* u : M::attr(a, "units")->k) {
        const std::string nat = M::attr(u, "nation")->k[1]->s;
        if ((FP.count(nat) ? 0 : 1) != side) continue;
        ++out.units[{t.nation_out.at(nat), M::attr(u, "kind")->k[1]->s == "REGULAR" ? "regular" : "elite"}];
    }
    for (Obj* ld : M::attr(a, "leaders")->k) {
        const std::string tn = M::type(ld);
        if (tn == "game_state.Nazgul") { if (side == 1) ++out.units[{"SAURON", "nazgul"}]; }
        else if (tn == "game_state.Leader") {
            const std::string nat = M::attr(ld, "nation")->k[1]->s;
            if ((FP.count(nat) ? 0 : 1) == side) ++out.units[{t.nation_out.at(nat), "leader"}];
        } else {
            Obj** n = M::attr_slot(ld, "name");
            if (!n || (*n)->tag != WOTR_STRING) continue;
            std::string ck;
            for (const auto& kv : t.char_in) if (kv.second == (*n)->s) ck = kv.first;
            if (ck.empty()) continue;
            const bool minion = std::find(t.minion_keys.begin(), t.minion_keys.end(), ck) != t.minion_keys.end();
            if (minion == (side == 1)) out.chars.insert(ck);
        }
    }
    return out;
}

// army_move_plan: the action applied on a copy (the box's apply_native_descriptor, as ai._execute_action), the
// figures that left its region, whether that was everything that can move (Saruman never moves)
Adapter::MovePlan Adapter::army_move_plan(Obj* st, const Row& a, const Memo& memo) {
    MovePlan plan;
    const std::string frm = pstr_of(a.params, "from");
    // the menu's own snapshot of st when the row carries it (st is the state the menu was worked out on: encoding it
    // again for every row of a long menu, an advance's unit splits, took seconds)
    void* own_sn = nullptr;
    if (!a.sn) {
        const pyobj::Graph g = m.encode(st);
        own_sn = wotr_snapshot_create(g.values.data(), g.values.size(), g.edges.data(), g.edges.size(), g.bytes.data(), g.bytes.size(), g.root);
        if (!own_sn) return plan;
    }
    const void* sn = a.sn ? a.sn : own_sn;
    // apply_native_random_descriptor (the native _execute_action) with a stream of its own: the actions planned
    // here (army moves, victor advances) draw no random number
    const uint32_t limb = 1;
    void* rng = wotr_random_create_seeded(&limb, 1);
    void* rng_out = nullptr; void* events = nullptr;
    void* after = (a.menu && obs_tables && rng) ? wotr_snapshot_apply_descriptor_random(sn, obs_tables, a.menu, rng, memo.side, a.index, &rng_out, &events) : nullptr;
    if (!after && std::getenv("AW_BOT_DEBUG")) std::fprintf(stderr, "[plan] army_move_plan: apply failed: %s\n", wotr_snapshot_last_error());
    if (rng_out) wotr_random_destroy(rng_out);
    if (events) wotr_snapshot_destroy(events);
    if (rng) wotr_random_destroy(rng);
    if (own_sn) wotr_snapshot_destroy(own_sn);
    if (!after) {                                             // ai._execute_action raised: no plan
        return plan;
    }
    pyobj::Graph ag;
    const size_t nv = wotr_snapshot_node_count(after), ne = wotr_snapshot_edge_count(after), nb = wotr_snapshot_byte_count(after);
    ag.values.resize(nv); ag.edges.resize(ne); ag.bytes.resize(nb); ag.root = wotr_snapshot_root(after);
    wotr_snapshot_export(after, ag.values.data(), nv, ag.edges.data(), ne, ag.bytes.data(), nb);
    wotr_snapshot_destroy(after);
    Obj* w = m.decode(ag);
    const Figures before = figures(st, frm, memo.side), now = figures(w, frm, memo.side);
    std::map<std::pair<std::string, std::string>, int64_t> moved;
    for (const auto& [k, n] : before.units) { auto it = now.units.find(k); const int64_t d = n - (it == now.units.end() ? 0 : it->second); if (d > 0) moved[k] = d; }
    std::set<std::string> chars;
    for (const auto& c : before.chars) if (!now.chars.count(c)) chars.insert(c);
    if (moved.empty() && chars.empty()) return plan;
    std::set<std::string> movable = before.chars;
    movable.erase("saruman");
    plan.ok = true;
    plan.everything = moved == before.units && chars == movable;
    plan.moved = moved; plan.chars = chars;
    return plan;
}

// recruit_plan, companion_plan, minion_plan (the action's plan, made at once in Python: here when asked)
std::function<std::shared_ptr<Plan>()> Adapter::plan_maker(Obj* st, const Row& a, const Memo& memo) {
    using M = pyobj::Model;
    const Value& p = a.params;
    const std::string ty = a.type;
    if (ty == "muster_recruit" || ty == "muster_elite") {
        const std::string nm = pstr_of(p, "nation");
        auto it = t.nation_out.find(nm);
        const Value nat = it == t.nation_out.end() ? Value() : S(it->second);
        std::string unit;
        if (ty == "muster_elite") unit = "elite";
        else {
            const Value* tp = p.get("type");
            unit = !tp ? "regular" : tp->kind == Value::Null ? "None" : tp->kind == Value::Str ? pstr_of(p, "type") : std::to_string(tp->as_int());
            if (unit == "leader" && nat.kind == Value::Str && nat.s == "SAURON") unit = "nazgul";
        }
        const Value region = slugv(at(p, "region")), u = S(unit);
        return [region, nat, u]() { return std::make_shared<RecruitPlan>(region, nat, u); };
    }
    const Value& frm = at(p, "from").truthy() ? at(p, "from") : at(p, "from_region");
    const Value& to = at(p, "to").truthy() ? at(p, "to") : at(p, "to_region");
    const std::string fkey = frm.kind == Value::Str ? pstr_of(p, at(p, "from").truthy() ? "from" : "from_region") : "";
    Obj* army = fkey.empty() ? nullptr : army_at(st, fkey);
    if (ty == "companion_move") {
        std::vector<std::string> who;
        if (at(p, "companions").truthy()) { for (const Value& n : at(p, "companions").arr) if (n.kind == Value::Str) who.push_back(n.s); }
        else if (army) for (Obj* ld : M::attr(army, "leaders")->k) if (M::type(ld) == "game_state.Companion") who.push_back(M::attr(ld, "name")->s);
        std::vector<std::string> keys;
        for (const auto& n : who) { const std::string k = char_out(n); if (!k.empty()) keys.push_back(k); }
        const Value f = slugv(frm), tt = slugv(to);
        return [keys, f, tt]() { return std::make_shared<CompanionMovePlan>(keys, f, tt); };
    }
    // nazgul_move / mouth_move
    const Value f = slugv(frm), tt = slugv(to);
    std::vector<std::tuple<std::string, Value, Value>> flights;
    if (ty == "mouth_move") flights.push_back({"mouth-of-sauron", f, tt});
    else {
        int64_t n = 0;
        if (army) for (Obj* ld : M::attr(army, "leaders")->k) n += M::type(ld) == "game_state.Nazgul";
        if (p.get("count") && !at(p, "count").is_null()) n = std::min(n, at(p, "count").as_int());
        if (at(p, "include_wk").truthy()) flights.push_back({"witch-king", f, tt});
        for (int64_t i = 0; i < n; ++i) flights.push_back({"nazgul", f, tt});
    }
    const Tables* tp = &t;
    return [tp, flights]() { return std::make_shared<MinionPlan>(*tp, flights); };
}

std::shared_ptr<Plan> Adapter::army_move_plan_obj(Obj* st, const Row& a, const Memo& memo, bool advance) {
    const MovePlan mp = army_move_plan(st, a, memo);
    if (!mp.ok) return nullptr;
    return std::make_shared<ArmyMovePlan>(t, slugv(at(a.params, "from")), slugv(at(a.params, "to")), mp.moved, mp.chars, mp.everything, advance);
}

Value Adapter::slugv(const Value& key) const {
    if (key.kind == Value::Null) return Value();
    const std::string k = key.kind == Value::Str ? (key.s.rfind("\x01" "enum:", 0) == 0 ? key.s.substr(6) : key.s) : std::to_string(key.as_int());
    const std::string sl = slug(k);
    return sl.empty() ? Value() : S(sl);
}

std::string Adapter::char_out(const std::string& name) const {
    for (const auto& kv : t.char_in) if (kv.second == name) return kv.first;
    return "";
}

std::string Adapter::pstr_of(const Value& v, const char* k) {
    const Value& x = at(v, k);
    if (x.kind != Value::Str) return "";
    return x.s.rfind("\x01" "enum:", 0) == 0 ? x.s.substr(6) : x.s;
}

// choice_tokens: the protocol names a card option of the engine mentions
void Adapter::choice_tokens(const Value& v, std::set<std::pair<std::string, std::string>>& out) {
    if (v.kind == Value::Obj) { for (const auto& kv : v.obj) if (kv.first.rfind("_", 0) != 0) choice_tokens(kv.second, out); return; }
    if (v.kind == Value::Arr) { for (const auto& x : v.arr) choice_tokens(x, out); return; }
    if (v.kind == Value::Bool) { out.insert({"bool", v.b ? "1" : "0"}); return; }
    if (v.kind == Value::Int) { out.insert({"int", std::to_string(v.i)}); return; }
    if (v.kind == Value::Str && v.s.rfind("\x01" "enum:", 0) == 0) {          // an Enum: its protocol nation, if any
        const std::string member = v.s.substr(6);
        auto it = t.nation_out.find(member);
        if (it != t.nation_out.end()) out.insert({"nation", it->second});
        return;
    }
    const std::string s = v.kind == Value::Str ? v.s : (v.kind == Value::Null ? "None" : (v.kind == Value::Real ? std::to_string(v.d) : ""));
    const std::string sl = slug(s);
    if (!sl.empty()) out.insert({"region", sl});
    for (const auto& kv : t.char_in) if (kv.second == s) out.insert({"character", kv.first});
    auto ck = t.card_out.find(s);
    if (ck != t.card_out.end()) out.insert({"card", ck->second});
    for (const auto& kv : t.nation_in) {
        const std::string ps = plain(s);
        if (ps == plain(kv.first) || ps == plain(t.nation_value.count(kv.second) ? t.nation_value.at(kv.second) : "") || ps == plain(kv.second))
            out.insert({"nation", kv.first});
    }
    static const std::set<std::string> DECLINE = {"none", "skip", "decline", "no", "pass", "done", "stay"};
    if (DECLINE.count(lower(s))) out.insert({"decline", "1"});
    out.insert({"word", plain(s)});
}

// score_options: the referee's card answer that best fits the engine's choice (-1: none)
int64_t Adapter::score_options(const Value& dec, const std::set<std::pair<std::string, std::string>>& toks, bool empty) {
    bool have = false; int64_t best_s = 0, best_o = -1;
    const bool decline = toks.count({"decline", "1"}) || empty;
    for (const Value& o : or_empty(dec, "options").arr) {
        int64_t s = 0;
        for (const auto& [field, tk] : std::vector<std::pair<std::string, std::string>>{
                 {"region", "region"}, {"from", "region"}, {"to", "region"}, {"character", "character"}, {"nation", "nation"}, {"target_card", "card"}}) {
            const Value& x = at(o, field);
            if (x.kind != Value::Null) s += toks.count({tk, x.kind == Value::Str ? x.s : std::to_string(x.i)}) ? 3 : -1;
        }
        if (at(o, "count").kind != Value::Null) s += toks.count({"int", std::to_string(at(o, "count").as_int())}) ? 3 : -1;
        if (o.get("yes")) s += (at(o, "yes").truthy() != decline) ? 1 : -1;
        if (at(o, "skip").truthy()) s += decline ? 2 : -2;
        std::string ans = at(o, "answer").kind == Value::Str ? at(o, "answer").s : "";
        for (char& c : ans) if (c == '>' || c == ':') c = ' ';
        size_t i = 0;
        while (i < ans.size()) {
            while (i < ans.size() && std::isspace(static_cast<unsigned char>(ans[i]))) ++i;
            size_t j = i;
            while (j < ans.size() && !std::isspace(static_cast<unsigned char>(ans[j]))) ++j;
            if (j > i) {
                const std::string part = ans.substr(i, j - i);
                if (toks.count({"region", part}) || toks.count({"card", part})) s += 1;
                if (toks.count({"word", plain(part)})) s += 1;
            }
            i = j;
        }
        if (!have || s > best_s) { have = true; best_s = s; best_o = at(o, "o").kind == Value::Null ? -1 : at(o, "o").as_int(); }
    }
    if (!have || best_s <= 0) return -1;
    return best_o;
}

// ---- the board diff (protocol/wotr_protocol_bot.py figure_counts, board_diff, diff_pick, question_region) ----
namespace {
std::string text_of(const Value& v) { return v.kind == Value::Str ? v.s : v.kind == Value::Null ? std::string("\x01") : v.kind == Value::Int ? std::to_string(v.i) : std::string("\x02"); }
}  // namespace

void Adapter::figure_counts(Obj* st, std::vector<std::pair<DiffKey, int64_t>>& out, std::set<std::pair<std::string, std::string>>& chars) {
    using M = pyobj::Model;
    Obj* armies = M::attr(st, "armies");
    for (size_t j = 0; j + 1 < armies->k.size(); j += 2) {
        const std::string k = armies->k[j]->s;
        std::string sl = slug(k);
        if (sl.empty()) sl = "\x01";
        for (int side : {0, 1}) {
            const auto fg = figures(st, k, side);
            for (const auto& [nu, n] : fg.units) {
                const DiffKey key{sl, nu.first, nu.second};
                auto it = std::find_if(out.begin(), out.end(), [&](auto& e) { return e.first == key; });
                if (it == out.end()) out.push_back({key, n}); else it->second += n;
            }
            for (const auto& c : fg.chars) chars.insert({sl, c});
        }
    }
}

Diff Adapter::board_diff(Obj* before, Obj* after) {
    std::vector<std::pair<DiffKey, int64_t>> b, w;
    std::set<std::pair<std::string, std::string>> bc, wc;
    figure_counts(before, b, bc);
    figure_counts(after, w, wc);
    Diff d;
    std::vector<DiffKey> keys;   // set(b) | set(w) (here: b's keys, then w's new ones)
    for (auto& e : b) keys.push_back(e.first);
    for (auto& e : w) if (std::find(keys.begin(), keys.end(), e.first) == keys.end()) keys.push_back(e.first);
    auto get = [](const std::vector<std::pair<DiffKey, int64_t>>& v, const DiffKey& k) { for (auto& e : v) if (e.first == k) return e.second; return int64_t(0); };
    for (const auto& k : keys) {
        const int64_t dd = get(w, k) - get(b, k);
        if (dd > 0) d.added.push_back({k, dd}); else if (dd < 0) d.removed.push_back({k, -dd});
    }
    for (auto& c : wc) if (!bc.count(c)) d.chars_to.insert(c);
    for (auto& c : bc) if (!wc.count(c)) d.chars_from.insert(c);
    return d;
}

std::string Adapter::question_region(const Value& question) const {
    const std::string q = question.kind == Value::Str ? question.s : "";
    const size_t at_ = q.rfind(" in ");
    if (at_ == std::string::npos) return "";
    std::string tail = q.substr(at_ + 4);
    while (!tail.empty() && (tail.back() == '?' || std::isspace(static_cast<unsigned char>(tail.back())))) tail.pop_back();
    size_t i = 0; while (i < tail.size() && std::isspace(static_cast<unsigned char>(tail[i]))) ++i;
    // plain(): letters and digits only, accents folded (NFD then ASCII): the protocol's names carry Latin-1 accents
    // as UTF-8, whose bytes are dropped (a folded letter's base letter is kept by the NFD in Python: here the
    // two-byte sequences of the accented vowels the board uses are mapped to their base letter)
    auto fold = [](const std::string& x) {
        std::string o;
        for (size_t j = 0; j < x.size(); ++j) {
            const unsigned char c = static_cast<unsigned char>(x[j]);
            if (c == 0xC3 && j + 1 < x.size()) {
                const unsigned char d = static_cast<unsigned char>(x[++j]);
                static const char* map = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00E0..U+00FF
                static const char* up = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs";    // U+00C0..U+00DF
                char base = 0;
                if (d >= 0xA0 && d <= 0xBF) base = map[d - 0xA0]; else if (d >= 0x80 && d <= 0x9F) base = up[d - 0x80];
                if (base && std::isalnum(static_cast<unsigned char>(base))) o += char(std::tolower(static_cast<unsigned char>(base)));
                continue;
            }
            if (c < 128 && std::isalnum(c)) o += char(std::tolower(c));
        }
        return o;
    };
    const std::string want = fold(tail.substr(i));
    for (const auto& kv : t.region_in) if (fold(kv.first) == want) return kv.first;
    return "";
}

std::optional<int64_t> Adapter::diff_pick(const Value& dec, Diff& d, const PickCtx& c) {
    using M = pyobj::Model;
    const std::string kind = at(dec, "kind").s;
    const Value& ctx = or_empty(dec, "context");
    auto o_of = [](const Value* o) { return has_o(o) ? std::optional<int64_t>(at(*o, "o").as_int()) : std::nullopt; };
    auto any_region = [](const std::vector<std::pair<DiffKey, int64_t>>& v, const std::string& r, bool pos) {
        for (auto& e : v) if (std::get<0>(e.first) == r && (!pos || e.second > 0)) return true; return false; };
    if (kind == "SEPARATE_COMPANIONS") {   // a card's separation the engine made inside the card (I Will Go Alone)
        std::map<std::string, std::string> to;
        for (const auto& [r, ch] : d.chars_to) to.emplace(ch, r);
        for (auto* o : opts(dec, "separate")) {
            const std::string ch = text_of(at(*o, "character"));
            if (to.count(ch) && std::find(d.separated.begin(), d.separated.end(), ch) == d.separated.end()) { d.separated.push_back(ch); return o_of(o); }
        }
        for (auto* o : opts(dec, "separate_to")) {
            const std::string tt = text_of(at(*o, "to"));
            for (const auto& ch : d.separated) { auto it = to.find(ch); if (it != to.end() && it->second == tt) return o_of(o); }
        }
        return std::nullopt;
    }
    if (kind == "ARMY_MOVE") {   // a card's Army moves the engine made inside the card
        if (c.work_action != "play_event") return std::nullopt;
        int best_s = -1; std::optional<int64_t> best_o; std::pair<std::string, std::string> best_m;
        for (auto* o : opts(dec, "army_move")) {
            const std::string f = text_of(at(*o, "from")), to = text_of(at(*o, "to"));
            if (d.moves_done.count({f, to})) continue;
            const int s = 2 * any_region(d.removed, f, true) + 2 * any_region(d.added, to, true);
            if (s >= 4 && (best_s < 0 || s > best_s)) { best_s = s; best_o = o_of(o); best_m = {f, to}; }
        }
        if (best_s >= 0) { d.moves_done.insert(best_m); return best_o; }
        const auto e = opts(dec, "end");
        return e.empty() ? std::nullopt : o_of(e[0]);
    }
    if (kind == "ARMY_SELECTION") {   // a card's Army move: as many as left the region in the engine
        const std::string frm = text_of(at(ctx, "from"));
        const Value* best = nullptr; int64_t bd = 0;
        auto nat_of = [&](const Value& o) { const Value& x = at(o, "nation"); return text_of(x.truthy() ? x : at(ctx, "nation")); };
        auto unit_of = [&](const Value& o) { const Value& x = at(o, "unit"); return text_of(x.truthy() ? x : at(ctx, "unit")); };
        for (auto* o : opts(dec, "count")) {
            int64_t want = 0;
            if (at(*o, "character").truthy()) want = d.chars_from.count({frm, text_of(at(*o, "character"))}) ? 1 : 0;
            else { const DiffKey k{frm, nat_of(*o), unit_of(*o)}; for (auto& e : d.removed) if (e.first == k) want = e.second; }
            const int64_t dd = std::llabs((o->get("count") ? at(*o, "count").as_int() : 0) - want);
            if (!best || dd < bd) { best = o; bd = dd; }
        }
        if (!best) return std::nullopt;
        if (!at(*best, "character").truthy()) {
            const DiffKey k{frm, nat_of(*best), unit_of(*best)};
            for (auto& e : d.removed) if (e.first == k) e.second = std::max<int64_t>(0, e.second - (best->get("count") ? at(*best, "count").as_int() : 0));
        }
        return o_of(best);
    }
    // a card's question: the words of its question, the answers' words
    std::string q = at(ctx, "question").kind == Value::Str ? at(ctx, "question").s : (at(ctx, "question").is_null() ? "" : "?");
    if (!ctx.get("question")) q = "";
    for (char& ch : q) if (ch == '?') ch = ' ';
    std::vector<std::string> words;
    { size_t i = 0; while (i < q.size()) { while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i; size_t j = i; while (j < q.size() && !std::isspace(static_cast<unsigned char>(q[j]))) ++j; if (j > i) words.push_back(q.substr(i, j - i)); i = j; } }
    std::string region = "\x03", nation = "\x03";   // \x03: None (any)
    for (auto& wd : words) if (t.region_in.count(wd)) { region = wd; break; }
    for (auto& wd : words) if (t.nation_in.count(wd)) { nation = wd; break; }
    std::vector<const Value*> counts;
    for (auto* o : opts(dec, "card_choice")) if (o->get("count") && !at(*o, "count").is_null()) counts.push_back(o);
    const std::string where = question_region(at(ctx, "question"));
    if (!counts.empty() && !where.empty()) {
        // a recruitment card's count in one region ("How many Regulars in <region>?"): as many as the engine put
        // there; that region is then done
        int64_t want = 0;
        for (auto& e : d.added) if (std::get<0>(e.first) == where) want += e.second;
        const Value* best = counts[0];
        auto key = [&](const Value* o) { const int64_t n = at(*o, "count").as_int(); return std::make_pair(std::llabs(n - want), n); };
        for (auto* o : counts) if (key(o) < key(best)) best = o;
        d.added.erase(std::remove_if(d.added.begin(), d.added.end(), [&](auto& e) { return std::get<0>(e.first) == where; }), d.added.end());
        return o_of(best);
    }
    static const std::map<std::string, std::string> UNIT_ANSWERS = {{"REGULAR", "regular"}, {"ELITE", "elite"}, {"LEADER", "leader"}, {"NAZGUL", "nazgul"}};
    auto upper = [](std::string x) { for (char& ch : x) ch = char(std::toupper(static_cast<unsigned char>(ch))); return x; };
    auto answer_text = [](const Value& o) { const Value* a = o.get("answer"); return !a ? std::string() : a->kind == Value::Str ? a->s : a->is_null() ? std::string("None") : a->kind == Value::Int ? std::to_string(a->i) : a->kind == Value::Bool ? std::string(a->b ? "True" : "False") : std::string("?"); };
    std::vector<const Value*> unit_opts;
    for (auto* o : opts(dec, "card_choice")) if (UNIT_ANSWERS.count(upper(answer_text(*o)))) unit_opts.push_back(o);
    if (!unit_opts.empty()) {
        // ("Recruit which Sauron figure in Dol Guldur?": REGULAR / ELITE / skip, asked once per figure)
        if (region == "\x03" && !where.empty()) region = where;
        for (auto* o : unit_opts) {
            const std::string unit = UNIT_ANSWERS.at(upper(answer_text(*o)));
            for (auto& e : d.added) {
                if (e.second > 0 && std::get<2>(e.first) == unit && (region == "\x03" || region == std::get<0>(e.first)) &&
                    (nation == "\x03" || nation == std::get<1>(e.first))) {
                    --e.second;
                    return o_of(o);
                }
            }
        }
        const auto sk = opts(dec, "card_choice", {f("skip", B(true))});   // the engine put no more of them there
        return sk.empty() ? std::nullopt : o_of(sk[0]);
    }
    int64_t best_s = 0; std::optional<int64_t> best_o; bool found = false;
    for (auto* o : opts(dec, "card_choice")) {
        int64_t s = 0;
        const Value& rv = at(*o, "region");
        if (!rv.is_null()) {
            const std::string r = text_of(rv);
            for (auto& e : d.added) if (std::get<0>(e.first) == r) s += e.second;
            for (auto& e : d.removed) if (std::get<0>(e.first) == r) s += e.second;
            for (auto& ch : d.chars_to) if (ch.first == r) s += 1;
        }
        Value fv = at(*o, "from"), tv = at(*o, "to");
        std::vector<std::string> path;
        { const std::string a = answer_text(*o); size_t i = 0; while (true) { size_t j = a.find('>', i); path.push_back(a.substr(i, j == std::string::npos ? std::string::npos : j - i)); if (j == std::string::npos) break; i = j + 1; } }
        if (fv.is_null() && path.size() > 1) { fv = S(path.front()); tv = S(path.back()); }
        if (!fv.is_null() && !tv.is_null()) {
            if (any_region(d.removed, text_of(fv), false)) s += 2;
            if (any_region(d.added, text_of(tv), false)) s += 2;
            for (size_t i = 1; i + 1 < path.size(); ++i) if (any_region(d.added, path[i], false)) s += 1;
        }
        const Value& cv = at(*o, "character");
        if (!cv.is_null()) {
            const std::string ch = text_of(cv);
            for (auto& e : d.chars_to) if (e.second == ch) s += 2;
            for (auto& e : d.chars_from) if (e.second == ch) s += 1;
        }
        if (s > 0 && (!found || s > best_s)) { found = true; best_s = s; best_o = o_of(o); }
    }
    if (!found && kind == "CARD_CHOICE" && d.added.empty() && d.removed.empty() && d.chars_to.empty() && d.chars_from.empty() &&
        at(ctx, "card").kind == Value::Str && !c.work_card.empty() && at(ctx, "card").s == c.work_card && c.work) {
        // the engine's card had no effect on the board (Half-orcs and Goblin-men with no Isengard figure left), and
        // it asks nothing more of this card (no pending option of its own; another one may be open)
        Obj** pco = M::attr_slot(c.work, "pending_card_option");
        std::string pcard;
        if (pco && (*pco)->tag == WOTR_DICT) { Obj* id = M::dict_get(*pco, "card_id"); if (id && id->tag == WOTR_STRING) { auto it = t.card_out.find(id->s); if (it != t.card_out.end()) pcard = it->second; } }
        if (pcard != c.work_card) {
            auto sk = opts(dec, "card_choice", {f("skip", B(true))});
            if (sk.empty()) sk = opts(dec, "card_choice");
            if (!sk.empty() && has_o(sk[0])) return o_of(sk[0]);
        }
    }
    return found ? best_o : std::nullopt;
}

pyobj::Obj* Adapter::apply_row(Obj* st, const Row& a, int side) {
    void* own_sn = nullptr;
    if (!a.sn) {
        const pyobj::Graph g = m.encode(st);
        own_sn = wotr_snapshot_create(g.values.data(), g.values.size(), g.edges.data(), g.edges.size(), g.bytes.data(), g.bytes.size(), g.root);
        if (!own_sn) return nullptr;
    }
    const void* sn = a.sn ? a.sn : own_sn;
    const uint32_t limb = 1;
    void* rng = wotr_random_create_seeded(&limb, 1);
    void* rng_out = nullptr; void* events = nullptr;
    void* after = (a.menu && obs_tables && rng) ? wotr_snapshot_apply_descriptor_random(sn, obs_tables, a.menu, rng, side, a.index, &rng_out, &events) : nullptr;
    if (rng_out) wotr_random_destroy(rng_out);
    if (events) wotr_snapshot_destroy(events);
    if (rng) wotr_random_destroy(rng);
    if (own_sn) wotr_snapshot_destroy(own_sn);
    if (!after) return nullptr;
    pyobj::Graph ag;
    const size_t nv = wotr_snapshot_node_count(after), ne = wotr_snapshot_edge_count(after), nb = wotr_snapshot_byte_count(after);
    ag.values.resize(nv); ag.edges.resize(ne); ag.bytes.resize(nb); ag.root = wotr_snapshot_root(after);
    wotr_snapshot_export(after, ag.values.data(), nv, ag.edges.data(), ne, ag.bytes.data(), nb);
    wotr_snapshot_destroy(after);
    return m.decode(ag);
}

}  // namespace awad
