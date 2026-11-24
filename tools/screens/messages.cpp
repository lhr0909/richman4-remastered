// SPDX-License-Identifier: GPL-3.0-or-later
// Layout, timing and texts follow docs/ui/messages.md, which cites the original asm
// (_rich4_player_say 0x44ef41, fcn_00440cac/00440ba8, _rich4_ui_yesno, rich4_news.asm,
// rich4_fortune.asm, rich4_gods.asm).
#include "messages.hpp"

#include "../native_stock_names.hpp"
#include "rich4/services.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace rich4::native::messages {
namespace {
struct Line {
    std::uint16_t voice = 0; // Speaking.mkf entry from the line's "#NNNN" prefix; 0 = no line.
    std::string_view text;
};
#include "message_strings.inc"

constexpr auto near_black = ui::color(16, 16, 16); // #101010
constexpr auto near_white = ui::color(240, 240, 240); // #F0F0F0

// create_font(0x10, 0x101010, 0, 2, 1): regular 16 px near-black, no effect (bubble text).
constexpr ui::TextStyle bubble_style{16, near_black, false, false, false, 0};
// create_font(0x10, 0xf0f0f0, 0x101010, 3, 1): bold 16 px white with drop shadow (all boxes).
constexpr ui::TextStyle box_style{16, near_white, true, true, false, near_black};
// create_font(0x1c, 0xf0f0f0, 0x101010, 3, 0): bold 28 px white with shadow (news/fortune).
constexpr ui::TextStyle news_style{28, near_white, true, true, false, near_black};
constexpr ui::TextStyle news_row_style{24, near_white, true, true, false, near_black};
// create_font(0x1c, 0xffffff, 0x101010, 6, 0): bold 28 px white with 4-direction outline.
constexpr ui::TextStyle god_style{28, ui::color(255, 255, 255), true, false, true, near_black};

// Portrait frame - 1 per speech slot (§1.4).
constexpr std::array<std::uint8_t, 27> slot_moods{0, 0, 0, 2, 2, 2, 3, 3, 3, 2, 2, 2, 3, 3,
                                                  3, 0, 0, 0, 1, 2, 2, 1, 2, 2, 3, 2, 3};

// fortune_data_idx (csrc/fortune.c): photo entry per effective fortune handler.
constexpr std::array<std::uint16_t, 49> fortune_photos{
    0x01dd, 0x01de, 0x01df, 0x01e0, 0x01e1, 0x01e2, 0x01e3, 0x01e4, 0x01e5, 0x01e6, 0x01e7, 0x01e8, 0x01e9,
    0x01ea, 0x01eb, 0x01ec, 0x01ed, 0x01ee, 0x01ef, 0x01f0, 0x01f1, 0x01f1, 0x01f1, 0x01f2, 0x01f2, 0x01f3,
    0x01f4, 0x01f5, 0x01f5, 0x01f5, 0x01f6, 0x01f7, 0x01f8, 0x01f9, 0x01fa, 0x01fb, 0x01fc, 0x01f9, 0x01fd,
    0x01fe, 0x01ff, 0x0200, 0x01fa, 0x01fb, 0x0201, 0x0202, 0x0203, 0x01fe, 0x0204};

// 0x475ed8[0x475eb4[newsId]].
std::string_view news_title(unsigned id) {
    static constexpr std::array<std::string_view, 6> titles{"無責任新聞", "政府公告", "社會新聞",
                                                            "路況報導",   "氣象報導", "財經新聞"};
    return titles[id < 6 ? 0 : id < 14 ? 1 : id < 16 ? 2 : id < 18 ? 3 : id < 22 ? 4 : 5];
}

// Minimal printf for the original %s/%d formats (arguments consumed in order).
struct Argument {
    std::string text;
    Argument(std::string value) : text(std::move(value)) {}
    Argument(const char *value) : text(value) {}
    Argument(std::string_view value) : text(value) {}
    Argument(std::int64_t value) : text(std::to_string(value)) {}
    Argument(int value) : text(std::to_string(value)) {}
};
std::string format(std::string_view pattern, std::initializer_list<Argument> arguments) {
    std::string result;
    auto next = arguments.begin();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size() && (pattern[i + 1] == 's' || pattern[i + 1] == 'd')) {
            if (next != arguments.end())
                result += (next++)->text;
            ++i;
        } else
            result += pattern[i];
    }
    return result;
}

std::string player_name(const World &world, std::int64_t slot) {
    if (slot < 0 || std::size_t(slot) >= world.players.size())
        return "";
    return names.at(world.players[std::size_t(slot)].state.character % 12);
}
std::string strip_spaces(std::string_view text) {
    std::string result;
    for (const char c : text)
        if (c != ' ')
            result += c;
    return result;
}
std::string property_name(const World &world, std::uint16_t code) {
    if (code > 2000 && code < 4000 && std::size_t(code - 2000) < world.lands.size())
        return strip_spaces(ui::big5(world.lands[code - 2000].economy.street_name));
    if (code > 4000 && std::size_t(code - 4000) < world.facilities.size())
        return strip_spaces(ui::big5(world.facilities[code - 4000].name));
    return "";
}
std::string stock_name(const World &world, std::int64_t index) {
    const auto offset = std::size_t(world.header.stage_id * 4 + world.header.map_id) * 12;
    if (index < 0 || index >= 12 || offset + std::size_t(index) >= native_stock_names.size())
        return "";
    return strip_spaces(native_stock_names[offset + std::size_t(index)]);
}
std::string company_name(const World &world, std::int32_t company) {
    if (company <= 0 || std::size_t(company) >= world.commercials.size())
        return "";
    return stock_name(world, world.commercials[std::size_t(company)].stock_index);
}

} // namespace

Item speech_bubble(std::uint8_t character, unsigned category, std::optional<std::size_t> focus) {
    Item item;
    item.kind = Item::Kind::bubble;
    item.focus = focus;
    item.character = static_cast<std::uint8_t>(character % 12);
    item.mood = slot_moods.at(category);
    const auto line = event_lines.at(item.character).at(category);
    if (line.starts_with('@'))
        item.pictogram = std::stoi(std::string(line.substr(1)));
    else
        item.text = line;
    item.voice = 1050U + item.character * 27U + category;
    item.minimum_ms = 1000;
    item.wait_voice = true;
    return item;
}

std::optional<Item> item_speech_bubble(std::uint8_t character, SpeechTable table, unsigned slot,
                                       std::uint8_t mood, std::optional<std::size_t> focus) {
    character %= 12;
    Line line;
    if (table == SpeechTable::card && slot < 90)
        line = card_lines.at(character)[slot];
    else if (table == SpeechTable::tool && slot < 26)
        line = tool_lines.at(character)[slot];
    if (!line.voice)
        return std::nullopt;
    Item item;
    item.kind = Item::Kind::bubble;
    item.focus = focus;
    item.character = character;
    item.mood = static_cast<std::uint8_t>(mood % 4);
    if (line.text.starts_with('@'))
        item.pictogram = std::stoi(std::string(line.text.substr(1)));
    else
        item.text = line.text;
    item.voice = line.voice;
    item.minimum_ms = 1000;
    item.wait_voice = true;
    return item;
}

namespace {
Item bubble(const World &world, std::size_t slot, unsigned category) {
    return speech_bubble(world.players.at(slot).state.character, category, slot);
}

Item box(std::string text, std::uint32_t ms = 1500) {
    Item item;
    item.kind = Item::Kind::box;
    item.text = std::move(text);
    item.minimum_ms = ms;
    return item;
}

Item toll_box(const GameEvent &event, const World &world) {
    const auto amount = std::int64_t(event.amount);
    if (event.property > 4000) {
        // Facility charges (core_actions: hotel 0x4639ff, mall 0x463a14).
        if (event.announcement_id == 1)
            return box(format("休息%d天\n\n費用%d元！", {std::int64_t(event.value), amount}));
        if (event.announcement_id == 2 && event.value > 0)
            return box(format("您的消費金額為\n\n%dx%d倍=%d元",
                              {amount / event.value, std::int64_t(event.value), amount}));
    }
    // Gas stations charge 加油費 (fee table 0x475184; facility mapping UNVERIFIED); lands 過路費.
    const char *fee = event.property > 4000 && event.announcement_id == 3 ? "加油費" : "過路費";
    const auto place = property_name(world, event.property);
    if (event.second_subject >= 0)
        return box(format("%s\n\n屬%s與%s\n\n請付%d元%s",
                          {place, player_name(world, event.subject), player_name(world, event.second_subject),
                           amount, fee}));
    return box(format("%s\n\n此地屬%s\n\n請付%d元%s", {place, player_name(world, event.subject), amount, fee}));
}

Item news_item(const GameEvent &event, const World &world) {
    const unsigned id = event.announcement_id % 36;
    Item item;
    item.kind = Item::Kind::news;
    item.photo = 441 + id;
    item.title = news_title(id);
    item.voice = 149 + id;
    item.minimum_ms = 2400;
    item.wait_voice = true;
    const auto body = news_bodies[id];
    const auto place = property_name(world, event.property);
    if (id == 1 || id == 3)
        item.text = format(body, {std::int64_t(event.value)});
    else if (id >= 8 && id <= 10)
        item.text = format(body, {player_name(world, event.subject), std::int64_t(event.value)});
    else if (id == 27 || id == 28)
        item.text = format(body, {stock_name(world, event.subject)});
    else if (id == 29)
        item.text = format(body, {company_name(world, event.company), player_name(world, event.subject)});
    else if (id >= 30)
        item.text = format(body, {company_name(world, event.company)});
    else
        item.text = format(body, {place});
    for (std::size_t slot = 0; slot < std::min<std::size_t>(4, world.players.size()); ++slot) {
        if (!(event.players & (1U << slot)))
            continue;
        const auto character = static_cast<std::uint8_t>(world.players[slot].state.character % 12);
        if ((id >= 11 && id <= 13) || id == 23)
            item.rows.push_back({character, 3,
                                 format(id == 23 ? "%s得到%d元" : "%s繳交%d元",
                                        {player_name(world, std::int64_t(slot)),
                                         std::int64_t(event.player_amounts[slot])})});
        else if (id < 4 || id == 29)
            item.rows.push_back({character, 4, {}});
    }
    return item;
}

Item fortune_item(const GameEvent &event, const World &world) {
    const unsigned id = event.announcement_id % 49;
    Item item;
    item.kind = Item::Kind::fortune;
    item.photo = fortune_photos[id];
    item.voice = 185 + id;
    item.minimum_ms = 1600;
    item.wait_voice = true;
    item.text = format(fortune_bodies[id], {std::int64_t(event.value)});
    if (event.player < world.players.size())
        item.character = static_cast<std::uint8_t>(world.players[event.player].state.character % 12);
    return item;
}

std::optional<Item> god_item(const GameEvent &event) {
    const auto kind = event.announcement_id;
    const int index = kind >= 1 && kind <= 10 ? kind - 1 : kind == 12 ? 10 : kind == 15 ? 11 : -1;
    if (index < 0)
        return std::nullopt;
    Item item;
    item.kind = Item::Kind::god_caption;
    item.focus = event.player; // The god appears where the player stands (no AF12 animation yet).
    item.text = god_arrivals[std::size_t(index)];
    item.minimum_ms = 2400;
    return item;
}
} // namespace

std::optional<Item> from_event(const GameEvent &event, const World &world) {
    // Card/tool lines: the core already applied the original absent-speaker gate when emitting.
    if (event.speech_table != SpeechTable::event && event.player < world.players.size())
        return item_speech_bubble(world.players[event.player].state.character, event.speech_table,
                                  event.speech_slot, event.speech_mood, event.player);
    if (event.voice_category && event.player < world.players.size() && *event.voice_category < 27) {
        const auto &status = world.players[event.player].status;
        if (status.days_disappearing) // The original says nothing for an absent player.
            return std::nullopt;
        return bubble(world, event.player, *event.voice_category);
    }
    switch (event.announcement) {
    case Announcement::toll:
        return toll_box(event, world);
    case Announcement::cash_insufficient:
        return box("您的現金不足！");
    case Announcement::news:
        return news_item(event, world);
    case Announcement::fortune:
        return fortune_item(event, world);
    case Announcement::god_arrival:
        return god_item(event);
    case Announcement::absent_status: {
        static constexpr std::array<std::string_view, 5> patterns{
            "%s住宿中\n\n還剩%d天！", "%s消失中\n\n還剩%d天！", "%s坐牢中\n\n還剩%d天！",
            "%s住院中\n\n還剩%d天！", "%s冬眠中\n\n還剩%d天！"};
        if (event.value <= 0 || event.announcement_id >= patterns.size())
            return std::nullopt;
        return box(format(patterns[event.announcement_id],
                          {player_name(world, std::int64_t(event.player)), std::int64_t(event.value)}));
    }
    case Announcement::stock_card: // "對%s使用%s！" (0x4653ae), 1500 ms, a computer's 紅卡/黑卡
        if (event.subject < 0 || std::size_t(event.subject) >= world.stocks.size() ||
            (event.announcement_id != 24 && event.announcement_id != 25))
            return std::nullopt;
        return box("對" + stock_name(world, std::int64_t(event.subject)) + "使用" +
                   std::string(services::card_names[event.announcement_id - 1U]) + "！");
    case Announcement::stolen: { // "搶得%s的\n\n%s" (0x4652f8), 1500 ms, a computer's 搶奪卡
        const bool tool = event.announcement_id == 1;
        const auto id = std::size_t(event.value);
        if (id < 1 || id > (tool ? services::tool_names.size() : services::card_names.size()))
            return std::nullopt;
        return box("搶得" + player_name(world, event.subject) + "的\n\n" +
                   std::string(tool ? services::tool_names[id - 1] : services::card_names[id - 1]));
    }
    case Announcement::tool_use: // "使用%s" (0x4653e5), 1500 ms, before a computer's tool runs
        if (event.announcement_id < 1 || event.announcement_id > services::tool_names.size())
            return std::nullopt;
        return box("使用" + std::string(services::tool_names[event.announcement_id - 1U]));
    case Announcement::toll_waived: { // AUDIO item 4: _rich4_check_pay_toll_to_owner strings 0x463bb8..0x463c54
        static constexpr std::array<std::string_view, 9> patterns{
            "房屋查封中\n\n免收%s！", "與%s同盟中\n\n免收%s！", "死神顯靈\n\n免收%s！",
            "%s住宿中\n\n免收%s！",   "%s消失中\n\n免收%s！",   "%s坐牢中\n\n免收%s！",
            "%s住院中\n\n免收%s！",   "%s冬眠中\n\n免收%s！",   "%s夢遊中\n\n免收%s！"};
        if (event.announcement_id >= patterns.size())
            return std::nullopt;
        // Fee names 0x47517c[0x47528b[type]]: lands 過路費; facility type 1 住宿費, 2 購物費, 3 加油費.
        std::string_view fee = "過路費";
        if (event.property > 4000 && std::size_t(event.property - 4000) < world.facilities.size()) {
            const auto type = world.facilities[event.property - 4000].facility_type;
            fee = type == 1 ? "住宿費" : type == 2 ? "購物費" : type == 3 ? "加油費" : "過路費";
        }
        const auto pattern = patterns[event.announcement_id];
        if (event.announcement_id == 0 || event.announcement_id == 2)
            return box(format(pattern, {fee}));
        return box(format(pattern, {player_name(world, event.subject), fee}));
    }
    case Announcement::point_coupon:
        return box(event.amount == 50   ? "得點券５０點"
                   : event.amount == 30 ? "得點券３０點"
                                        : "得點券１０點",
                   1000);
    case Announcement::mini_game_points: // SG:4392: sprintf(0x463797 "得點券%d點"), fcn_00440cac(2000)
        return box("得點券" + std::to_string(event.amount) + "點", 2000);
    case Announcement::research_complete: { // fcn_0041c84f: sprintf(0x463b68 "%s開發完成！"), fcn_00440cac(1500)
        if (event.announcement_id < 1 || event.announcement_id > services::tool_names.size())
            return std::nullopt;
        auto item = box(std::string(services::tool_names[event.announcement_id - 1U]) + "開發完成！", 1500);
        if (event.player < world.players.size())
            item.focus = event.player;
        return item;
    }
    case Announcement::none:
        break;
    }
    return std::nullopt;
}

bool is_yes_no(const GameDecision &decision) {
    // Free card (0x465388) and single-target frame card (0x46534e) also use the yes/no box;
    // the core lists their YES option first.
    if (decision.kind == DecisionKind::card_target)
        return decision.options.size() == 2;
    const bool kind = decision.kind == DecisionKind::buy_land || decision.kind == DecisionKind::upgrade_land ||
                      decision.kind == DecisionKind::buy_facility ||
                      decision.kind == DecisionKind::upgrade_facility;
    if (!kind || decision.options.size() != 2)
        return false;
    const auto has = [&](int value) {
        return std::any_of(decision.options.begin(), decision.options.end(),
                           [value](const auto &option) { return option.value == value; });
    };
    return has(1) && has(0);
}

std::string yes_no_question(const GameDecision &decision, const World &world) {
    if (decision.kind == DecisionKind::card_target) {
        const auto shown = localized(decision, world);
        return shown.prompt;
    }
    const bool facility = decision.kind == DecisionKind::buy_facility || decision.kind == DecisionKind::upgrade_facility;
    const auto place = property_name(world, static_cast<std::uint16_t>((facility ? 4000 : 2000) + decision.land_id));
    const bool upgrade = decision.kind == DecisionKind::upgrade_land || decision.kind == DecisionKind::upgrade_facility;
    return format(upgrade ? "%s\n\n升級費用:%d元\n\n是否升級？" : "%s\n\n費用:%d元\n\n是否買下此地？",
                  {place, std::int64_t(decision.amount)});
}

std::string subject_name(const World &world, const CardTarget &subject) {
    static constexpr std::array<std::string_view, 4> special_actors{"小偷", "強盜", "流氓", "間諜"};
    switch (subject.kind) {
    case CardTargetKind::actor:
        return subject.value < 4 ? player_name(world, subject.value)
                                 : subject.value < 8 ? std::string(special_actors[subject.value - 4]) : "";
    case CardTargetKind::property:
        return property_name(world, subject.value);
    case CardTargetKind::stock:
        return stock_name(world, subject.value);
    case CardTargetKind::object: {
        // Placed tools keep kind = tool + 14 (src/tools.cpp place()).
        if (subject.value == 0 || subject.value > world.objects.size())
            return "";
        const unsigned kind = world.objects[subject.value - 1U].kind;
        return kind >= 15 && kind - 15 < services::tool_names.size() ? std::string(services::tool_names[kind - 15])
                                                                     : "";
    }
    case CardTargetKind::card:
        return subject.value >= 1 && subject.value <= services::card_names.size()
                   ? std::string(services::card_names[subject.value - 1U])
                   : "";
    case CardTargetKind::tool:
        return subject.value >= 1 && subject.value <= services::tool_names.size()
                   ? std::string(services::tool_names[subject.value - 1U])
                   : "";
    }
    return "";
}

GameDecision localized(const GameDecision &decision, const World &world) {
    GameDecision shown = decision;
    for (auto &option : shown.options)
        if (option.subject)
            if (auto name = subject_name(world, *option.subject); !name.empty())
                option.label = decision.kind == DecisionKind::card_target ? "嫁禍給" + name : std::move(name);
    if (decision.kind == DecisionKind::card_target) {
        if (decision.options.size() == 2 && decision.options[0].subject) // 0x46534e
            shown.prompt = format("是否嫁禍給%s？", {subject_name(world, *decision.options[0].subject)});
        else if (decision.options.size() == 2) // 0x465388
            shown.prompt = format("%s\n\n是否使用免費卡？", {player_name(world, std::int64_t(decision.player))});
    }
    return shown;
}

void draw_text(Frame &frame, std::string_view text, int x, int y, int mode, const ui::TextStyle &style) {
    const auto extent = frame.font.measure_block(text, style);
    switch (mode) {
    case 0:
        frame.font.draw_block(frame.canvas, text, x, y, style);
        break;
    case 4:
        frame.font.draw_block(frame.canvas, text, x - extent.width / 2, y - extent.height / 2, style, true);
        break;
    case 5:
        frame.font.draw_block(frame.canvas, text, x, y - extent.height / 2, style);
        break;
    case 7:
        frame.font.draw_block(frame.canvas, text, x - extent.width / 2, y - extent.height, style, true);
        break;
    default:
        frame.font.draw_block(frame.canvas, text, x - extent.width / 2, y - extent.height / 2, style);
    }
}

void draw_yes_no(Frame &frame, std::string_view question, int yes_id, std::function<void()> yes, int no_id,
                 std::function<void()> no) {
    // fcn_00440ba8: dialog Data.mkf 517:5 anchored (220,140), text mode 4 at (220,140).
    graphics::blit(frame.canvas, frame.artwork.frame('d', 517, 5), 220, 140);
    draw_text(frame, question, 220, 140, 4, box_style);
    // _rich4_ui_yesno(220,320): Data.mkf 440 (96x48) centred → top-left (172,296), opaque.
    draw_yes_no_strip(frame, 220, 320, yes_id, std::move(yes), no_id, std::move(no));
}

void draw_yes_no_strip(Frame &frame, int centre_x, int centre_y, int yes_id, std::function<void()> yes, int no_id,
                       std::function<void()> no) {
    const auto &strip = frame.artwork.frame('d', 440, 0);
    const int left = centre_x - strip.width / 2, top = centre_y - strip.height / 2;
    // Touch: each half's hit area reaches 10 px beyond the strip.
    const graphics::Rect yes_rect{left - 10, top - 10, left + strip.width / 2, top + strip.height + 10};
    const graphics::Rect no_rect{left + strip.width / 2, top - 10, left + strip.width + 10, top + strip.height + 10};
    const std::size_t lit = frame.pointer_in(yes_rect) ? 1 : frame.pointer_in(no_rect) ? 2 : 0;
    const auto &shown = frame.artwork.frame('d', 440, lit);
    graphics::blit(frame.canvas, shown, left + shown.origin_x, top + shown.origin_y, graphics::Transparency::opaque);
    frame.hit(yes_id, yes_rect, std::move(yes));
    frame.hit(no_id, no_rect, std::move(no));
}

void Overlay::push(Item item) {
    if (item.kind == Item::Kind::bubble) {
        // Repeat suppression: the same line twice in a row is shown once.
        const auto key = std::to_string(item.voice);
        if (key == last_bubble_)
            return;
        last_bubble_ = key;
    }
    queue_.push_back(std::move(item));
}

void Overlay::clear() {
    if (started_ && audio_.stop_voice)
        audio_.stop_voice();
    queue_.clear();
    started_ = false;
}

void Overlay::skip() {
    if (queue_.empty())
        return;
    if (audio_.stop_voice)
        audio_.stop_voice();
    queue_.pop_front();
    started_ = false;
}

void Overlay::start(std::uint64_t now) {
    started_ = true;
    const auto &item = queue_.front();
    std::uint32_t voice_ms = 0;
    if (item.voice && audio_.speak)
        voice_ms = audio_.speak(item.voice);
    until_ = now + std::max(item.minimum_ms, item.wait_voice ? voice_ms : 0U);
}

void Overlay::retire() {
    if (started_ && !queue_.empty() && (instant_ || SDL_GetTicks() >= until_)) {
        queue_.pop_front();
        started_ = false;
    }
}

bool Overlay::draw(Frame &frame) {
    const std::uint64_t now = SDL_GetTicks();
    if (queue_.empty())
        return false;
    if (!started_)
        start(now);
    frame.buttons.clear(); // Modal: covered controls can never fire.
    draw_item(frame, queue_.front());
    frame.hit(skip_button, {0, 0, 640, 480}, [this] { skip(); });
    return true;
}

const graphics::Image &Overlay::photo(Frame &frame, std::size_t entry) {
    if (const auto found = photos_.find(entry); found != photos_.end())
        return found->second;
    graphics::Image image;
    image.width = 388;
    image.height = 251;
    const auto bytes = frame.artwork.archive('d').decode(entry);
    image.pixels.resize(std::size_t(image.width * image.height));
    for (std::size_t i = 0; i < image.pixels.size() && 2 * i + 1 < bytes.size(); ++i)
        image.pixels[i] = static_cast<std::uint16_t>(bytes[2 * i] | bytes[2 * i + 1] << 8);
    return photos_.emplace(entry, std::move(image)).first->second;
}

void Overlay::draw_item(Frame &frame, const Item &item) {
    auto &canvas = frame.canvas;
    switch (item.kind) {
    case Item::Kind::bubble: {
        // Cloud Data.mkf 517:6 at (220,130); only (0,40)-(440,260) is blitted to the screen.
        graphics::blit(canvas, frame.artwork.frame('d', 517, 6), 220, 130, {0, 40, 440, 260});
        graphics::blit(canvas, frame.artwork.frame('m', 27 + item.character, item.mood + 1U), 170, 130);
        if (item.pictogram > 0)
            graphics::blit(canvas, frame.artwork.frame('d', 519, std::size_t(item.pictogram - 1)), 240, 130);
        else
            draw_text(frame, item.text, 200, 130, 5, bubble_style);
        break;
    }
    case Item::Kind::box:
        graphics::blit(canvas, frame.artwork.frame('d', 517, 5), item.box_x, 140);
        draw_text(frame, item.text, item.box_x, 140, 4, box_style);
        break;
    case Item::Kind::news:
    case Item::Kind::fortune: {
        const bool news = item.kind == Item::Kind::news;
        const auto &background = frame.artwork.frame('p', 66, news ? 0 : 1);
        graphics::blit(canvas, background, background.origin_x, background.origin_y, graphics::Transparency::opaque);
        graphics::blit(canvas, photo(frame, item.photo), 25, 44, graphics::Transparency::opaque);
        if (news) {
            draw_text(frame, item.title, 24, 8, 0, news_style);
            draw_text(frame, item.text, 24, 310, 0, news_style);
            int k = 0;
            for (const auto &row : item.rows) {
                const auto &face = frame.artwork.frame('m', 27 + row.character, std::size_t(row.portrait_frame));
                if (row.text.empty())
                    graphics::blit(canvas, face, 390, 328 + 42 * k);
                else {
                    draw_text(frame, row.text, 24, 346 + 32 * k, 0, news_row_style);
                    graphics::blit(canvas, face, 390, 358 + 32 * k);
                }
                ++k;
            }
        } else {
            draw_text(frame, item.text, 24, 330, 0, news_style);
            graphics::blit(canvas, frame.artwork.frame('m', 27 + item.character, 2), 390, 344);
        }
        break;
    }
    case Item::Kind::god_caption:
        draw_text(frame, item.text, 220, 460, 7, god_style);
        break;
    }
}

} // namespace rich4::native::messages
