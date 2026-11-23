// SPDX-License-Identifier: GPL-3.0-or-later
// Map-tile info popup, after fcn_00417559 (rich4.asm:9646-10300). See tile_popup.hpp.
#include "tile_popup.hpp"

#include "messages.hpp"
#include "rich4/economy.hpp"
#include "rich4/services.hpp"

#include <array>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace rich4::native::tile_popup {
namespace {

// Original strings (Big5 in RICH4.EXE).
constexpr std::string_view empty_lot = "空  地";   // 0x463819
constexpr std::string_view chain_store = "連鎖店"; // 0x46390c
constexpr std::array<std::string_view, 6> land_levels{"空  地", "平  房", "店  舖",
                                                      "商  場", "商業大樓", "摩天大樓"}; // 0x475138
constexpr std::array<std::string_view, 5> facility_types{"公  園", "旅  館", "購物中心", "加油站",
                                                         "研究所"}; // 0x475150
constexpr std::array<std::string_view, 6> grades{"０級", "一級", "二級", "三級", "四級", "五級"}; // 0x475164
constexpr std::array<std::string_view, 4> special_players{"小偷", "強盜", "流氓", "間諜"}; // 0x47ed6a
// _rich4_god_names (0x47ed7a): map-object names by object kind 1..18.
constexpr std::array<std::string_view, 18> object_names{
    "小財神", "大財神", "小福神", "大福神", "小窮神", "大窮神", "小衰神", "大衰神", "天使",
    "惡魔",   "惡犬",   "土地公", "禮物",   "寶箱",   "死神",   "路障",   "地雷",   "定時炸彈"};

std::string text(std::string_view big5_bytes) { // drawn as stored (no strip_spaces here)
    return ui::big5(std::string(big5_bytes));
}
std::string player_name(const World &world, unsigned owner_code) {
    if (owner_code == 0 || owner_code > world.players.size())
        return "";
    return names.at(world.players[owner_code - 1].state.character % names.size());
}
std::string times(std::int64_t value) { // 0x463913 "×%d"
    return "×" + std::to_string(value);
}

struct Box {
    int left, top, right, bottom; // relative to the projected anchor
};
bool inside(const board::Point &anchor, const Box &box, int x, int y) {
    return x >= anchor.x + box.left && x <= anchor.x + box.right && y >= anchor.y + box.top &&
           y <= anchor.y + box.bottom;
}

} // namespace

std::uint16_t instance_at(const World &world, const board::Camera &camera, int x, int y) {
    const auto &viewport = camera.viewport;
    if (x < viewport.left || x >= viewport.right || y < viewport.top || y >= viewport.bottom)
        return 0;
    struct Hit {
        std::uint16_t code;
        int depth; // projected anchor y: the original draws larger y in front
        int distance;
    };
    std::optional<Hit> best;
    const auto consider = [&](std::uint16_t code, int world_x, int world_y, const Box &box) {
        const auto anchor = board::project({world_x, world_y}, camera);
        if (!anchor || !inside(*anchor, box, x, y))
            return;
        const Hit hit{code, anchor->y, std::abs(anchor->x - x) + std::abs(anchor->y + (box.top + box.bottom) / 2 - y)};
        if (!best || hit.depth > best->depth || (hit.depth == best->depth && hit.distance < best->distance))
            best = hit;
    };
    // Characters and map objects are sprites standing on their anchor; they are drawn over
    // the buildings around them, so they are tested first.
    for (std::size_t slot = 0; slot < world.players.size() && slot < 4; ++slot) {
        const auto &player = world.players[slot];
        if (player.state.x && player.state.controller_flags && !player.status.days_disappearing)
            consider(static_cast<std::uint16_t>(0x8000U | (1U << slot)), player.state.x, player.state.y,
                     {-16, -48, 16, 6});
    }
    for (std::size_t index = 0; index < world.special_actors.size() && index < 4; ++index) {
        const auto &actor = world.special_actors[index];
        if (actor.x && !actor.inactive_state && actor.node_id)
            consider(static_cast<std::uint16_t>(0x8000U | (0x10U << index)), actor.x, actor.y, {-16, -48, 16, 6});
    }
    for (std::size_t index = 0; index < world.objects.size() && index < 0x7f; ++index) {
        const auto &object = world.objects[index];
        if (object.node_id && !object.attached_player && object.node_id < world.nodes.size())
            consider(static_cast<std::uint16_t>(0x8000U | ((index + 1U) << 8U)), world.nodes[object.node_id].x,
                     world.nodes[object.node_id].y, {-14, -34, 14, 6});
    }
    if (best)
        return best->code;
    // Buildings: an empty lot is a flat marker, a building rises above its anchor.
    for (std::size_t id = 1; id < world.lands.size(); ++id) {
        const auto &land = world.lands[id];
        consider(static_cast<std::uint16_t>(2000 + id), land.x, land.y,
                 land.economy.level ? Box{-22, -50, 22, 12} : Box{-20, -14, 20, 12});
    }
    for (std::size_t id = 1; id < world.facilities.size(); ++id) {
        const auto &facility = world.facilities[id];
        consider(static_cast<std::uint16_t>(4000 + id), facility.x, facility.y,
                 facility.level ? Box{-30, -60, 30, 16} : Box{-26, -16, 26, 16});
    }
    for (std::size_t id = 1; id < world.commercials.size(); ++id)
        consider(static_cast<std::uint16_t>(6000 + id), world.commercials[id].x, world.commercials[id].y,
                 {-30, -60, 30, 16});
    for (std::size_t id = 1; id < world.landscapes.size(); ++id)
        if (!world.landscapes[id].name.empty()) // unnamed scenery shows nothing anyway
            consider(static_cast<std::uint16_t>(8000 + id), world.landscapes[id].x, world.landscapes[id].y,
                     {-30, -60, 30, 16});
    if (best)
        return best->code;
    // Road nodes: only named ones (the original tests the name's first byte, node +4).
    if (const auto tile = board::unproject({x, y}, camera))
        for (std::size_t id = 1; id < world.nodes.size() && id < 2000; ++id) {
            const auto &node = world.nodes[id];
            if (!node.name.empty() && (node.x >> 5) == (tile->x >> 5) && (node.y >> 5) == (tile->y >> 5))
                return static_cast<std::uint16_t>(id);
        }
    return 0;
}

std::vector<Line> lines(const World &world, std::uint16_t code) {
    std::vector<Line> result;
    const auto price_index = world.header.price_index;
    if (code > 0 && code < 2000) {
        if (code < world.nodes.size() && !world.nodes[code].name.empty())
            result.push_back({27, text(world.nodes[code].name)});
    } else if (code > 2000 && code < 4000) {
        if (std::size_t(code - 2000) >= world.lands.size())
            return result;
        const auto &land = world.lands[code - 2000];
        const auto &economy = land.economy;
        if (economy.owner_code)
            result.push_back({0, player_name(world, economy.owner_code)});
        const auto level = std::min<std::size_t>(economy.level, 5);
        result.push_back({18, std::string(level == 0           ? empty_lot
                                          : economy.chain_store ? chain_store
                                                                : land_levels[level])});
        result.push_back({36, std::string(grades[level])});
        std::string money;
        if (economy.owner_code) {
            std::vector<LandEconomyRecord> records;
            for (std::size_t i = 1; i < world.lands.size(); ++i)
                records.push_back(world.lands[i].economy);
            if (economy.chain_store) // $2000·price index ×(owner's chain stores)
                money = ui::money(std::int64_t(2000) * price_index) +
                        times(chain_store_count(records, economy.owner_code));
            else
                money = ui::money(land_toll(records, economy.owner_code, price_index,
                                            std::string_view(economy.street_name)));
        } else
            money = ui::money(economy.toll_by_level[level]); // land +0x20[level], as stored
        result.push_back({54, money});
    } else if (code > 4000 && code < 6000) {
        if (std::size_t(code - 4000) >= world.facilities.size())
            return result;
        const auto &facility = world.facilities[code - 4000];
        if (facility.owner_code)
            result.push_back({0, player_name(world, facility.owner_code)});
        if (!facility.level) {
            result.push_back({27, std::string(empty_lot)});
            return result;
        }
        const auto type = facility.facility_type;
        if (type > 4)
            return result;
        const auto level = std::min<std::size_t>(facility.level, 5);
        if (type == 0)
            result.push_back({27, std::string(facility_types[type])});
        else if (type == 3) {
            result.push_back({27, std::string(facility_types[type])});
            result.push_back({54, times(1000)});
        } else {
            result.push_back({18, std::string(facility_types[type])});
            result.push_back({36, std::string(grades[level])});
            if (type != 4) // hotel / mall: ×(facility +0x24[level] · price index)
                result.push_back({54, times(std::int64_t(facility.toll_by_level[level]) * price_index)});
            else if (facility.research_days() && facility.research_tool >= 1 && facility.research_tool <= 5)
                // Research institute (0x417ac2..0x417adf): show developing tool name when +0x1e != 0.
                result.push_back({54, std::string(services::tool_names[facility.research_tool + 7U])});
        }
    } else if (code > 6000 && code < 8000) {
        if (std::size_t(code - 6000) >= world.commercials.size())
            return result;
        const auto &company = world.commercials[code - 6000];
        if (company.owner_code)
            result.push_back({0, player_name(world, company.owner_code)});
        result.push_back({27, text(company.name)});
        result.push_back({54, "餘股:" + std::to_string(company.shares_remaining)}); // 0x463918
    } else if (code > 8000 && code < 10000) {
        if (std::size_t(code - 8000) < world.landscapes.size() && !world.landscapes[code - 8000].name.empty())
            result.push_back({27, text(world.landscapes[code - 8000].name)});
    } else if (code & 0x8000U) {
        if (code & 0xffU) {
            const unsigned slot = static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(code & 0xffU)));
            result.push_back({25, slot < 4 ? player_name(world, slot + 1) : std::string(special_players[slot - 4])});
        } else {
            const auto index = ((code & 0x7f00U) >> 8U);
            if (index >= 1 && index <= world.objects.size()) {
                const auto kind = world.objects[index - 1].kind;
                if (kind >= 1 && kind <= object_names.size())
                    result.push_back({25, std::string(object_names[kind - 1])});
            }
        }
    }
    return result;
}

void draw(Frame &frame, const World &world, const Popup &popup) {
    const auto text_lines = lines(world, popup.code);
    if (text_lines.empty())
        return;
    // q = (y - 40 >= h) + 2 * (x > 440 - w): the frame opens away from the screen edges. Each
    // frame's origin is its pointer corner, so anchoring it at the press point (fcn_00456418)
    // puts the pointer there.
    const auto &first = frame.artwork.frame('d', 517, 0);
    const int w = first.width, h = first.height;
    const int q = (popup.y - 40 >= h ? 1 : 0) + (popup.x > 440 - w ? 2 : 0);
    graphics::blit(frame.canvas, frame.artwork.frame('d', 517, std::size_t(q)), popup.x, popup.y);
    const int text_x = popup.x + ((q & 2) ? -(w / 2 + 9) : w / 2 + 9);
    const int base = popup.y + ((q & 1) ? -0x60 : 0x2a);
    // create_font(0x10, 0xf0f0f0, 0x101010, 3, 1): bold 16 px with a drop shadow; align 2.
    static constexpr ui::TextStyle style{16, ui::color(240, 240, 240), true, true, false, ui::color(16, 16, 16)};
    for (const auto &line : text_lines)
        messages::draw_text(frame, line.text, text_x, base + line.dy, 2, style);
}

} // namespace rich4::native::tile_popup
