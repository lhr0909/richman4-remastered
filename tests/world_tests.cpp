// SPDX-License-Identifier: GPL-3.0-or-later
#include "rich4/random.hpp"
#include "rich4/world.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
using Bytes = std::vector<std::uint8_t>;
using namespace rich4;
constexpr std::size_t map_offset = 0x274b;
void require(bool condition, const char *label) {
    if (!condition)
        throw std::runtime_error(label);
}
void put16(Bytes &bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}
void put32(Bytes &bytes, std::size_t offset, std::uint32_t value) {
    put16(bytes, offset, static_cast<std::uint16_t>(value));
    put16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16));
}
std::uint32_t u32(const Bytes &bytes, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= std::uint32_t(bytes.at(offset + i)) << (8 * i);
    return result;
}
template <class Function> void invalid(Function function) {
    try {
        function();
    } catch (const SaveError &) {
        return;
    }
    throw std::runtime_error("Expected malformed world rejection");
}
Bytes synthetic_map() {
    constexpr std::array<std::uint32_t, 5> counts{4, 1, 1, 1, 1};
    constexpr std::array<std::uint32_t, 5> strides{40, 52, 56, 52, 28};
    Bytes result(40);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const auto offset = result.size();
        put32(result, i * 8, counts[i]);
        put32(result, i * 8 + 4, static_cast<std::uint32_t>(offset));
        result.resize(offset + (counts[i] + 1) * strides[i], 0);
    }
    for (std::size_t id = 1; id <= 4; ++id) {
        const auto offset = 40 + id * 40;
        put16(result, offset, static_cast<std::uint16_t>(id * 64));
        put16(result, offset + 2, 240);
        put16(result, offset + 24, static_cast<std::uint16_t>(id == 1 ? 4 : id - 1));
        put16(result, offset + 26, static_cast<std::uint16_t>(id == 4 ? 1 : id + 1));
        put16(result, offset + 32, static_cast<std::uint16_t>(id == 1 ? 2001 : 0));
        put16(result, offset + 34, static_cast<std::uint16_t>(0x20 + id));
    }
    const auto land = u32(result, 12) + 52;
    result[land + 4] = 0xa4;
    result[land + 5] = 0x40;
    result[land + 7] = 0xef;  // Deliberately nonzero padding after the name's NUL.
    result[land + 20] = 0xd3; // Unknown byte must survive typed changes.
    put16(result, land + 28, 2300);
    put16(result, land + 30, 700);
    for (std::size_t i = 0; i < 6; ++i)
        put16(result, land + 32 + i * 2, static_cast<std::uint16_t>(100 + i * 25));
    const auto facility = u32(result, 20) + 56;
    result[facility + 4] = 'F';
    result[facility + 27] = 7;
    put16(result, facility + 34, 4700);
    put16(result, facility + 36, 1300);
    const auto commerce = u32(result, 28) + 52;
    result[commerce + 26] = 11;
    result[commerce + 27] = 3;
    put16(result, commerce + 32, 19);
    put16(result, commerce + 34, 5100);
    const auto landscape = u32(result, 36) + 28;
    result[landscape + 4] = 'L';
    result[commerce + 4] = 'C';
    result[40 + 2 * 40 + 4] = 'N'; // node 2
    result[landscape + 24] = 6;
    put16(result, landscape + 26, 444);
    return result;
}
Bytes synthetic_save() {
    const auto map = synthetic_map();
    Bytes bytes(map_offset + map.size() + 4 * (0x2718 + map.size()) + 7);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(i * 73 + i / 17);
    put32(bytes, 0, 0x26);
    put32(bytes, 12, 4);
    put32(bytes, 0x1b0, 1);
    put32(bytes, 0x2676, 2);
    put32(bytes, 0x2747, static_cast<std::uint32_t>(map.size()));
    std::copy(map.begin(), map.end(), bytes.begin() + map_offset);
    return bytes;
}

void lossless_and_patching() {
    const auto input = synthetic_save();
    auto world = World::from_save(SaveFile::decode(input));
    require(world.export_save() == input,
            "Typed decode preserves every source byte including snapshots/tail/pointers");
    require(&world.active_player() == &world.players[2], "Active player follows explicit current slot");
    require(world.lands[1].land_price == 2300 && world.lands[1].price_per_level == 700,
            "Original purchase +1c base/+1e building offsets (upstream struct labels reversed)");
    require(world.nodes[1].neighbors[0] == 4 && world.nodes[1].ground_sprite == 0x21,
            "Node topology and original ground SPR code");
    require(world.facilities[1].land_price == 4700 && world.facilities[1].price_per_level() == 1300 &&
                world.facilities[1].direction == 7 && world.commercials[1].commerce_type == 11 &&
                world.commercials[1].sprite_index == 19 && world.landscapes[1].sprite_index == 444,
            "All five original map tables retain their own layouts");
    auto expected = input;
    world.header.current_player = 1;
    put32(expected, 0x2676, 1);
    world.header.date = {29, 2, 2000};
    expected[4] = 29;
    expected[5] = 2;
    put16(expected, 6, 2000);
    world.players[1].state.cash = -123;
    put32(expected, 0x10 + 104 + 28, 0xffffff85U);
    world.players[1].state.node_id = 4;
    put16(expected, 0x10 + 104 + 12, 4);
    world.players[1].status.days_rejected_by_bank = 9;
    expected[0x10 + 104 + 59] = 9;
    world.players[1].status.card_strategy = 1;
    expected[0x10 + 104 + 22] = 1;
    world.players[1].status.loan_ratio = 73;
    expected[0x10 + 104 + 24] = 73;
    world.players[1].status.stock_investment_ratio = 81;
    expected[0x10 + 104 + 26] = 81;
    world.players[1].status.god_luck_modifier = -3;
    put16(expected, 0x10 + 104 + 68, 0xfffd);
    world.players[1].status.cumulative_income = 0xffaabcde;
    put32(expected, 0x10 + 104 + 96, 0xffaabcde);
    world.players[1].state.cards[0] = 30;
    expected[0x654 + 15] = 30;
    world.card_supply[29] = 0xff;
    expected[0x6cc + 29] = 0xff;
    world.news_deck[1] = 35;
    expected[0x26fa + 1] = 35;
    world.fortune_cursor = 36;
    put32(expected, 0x26f6, 36);
    world.hospital_roster[7] = 1;
    expected[0x26ea + 7] = 1;
    world.players[1].status.money_fortune_modifier = -123;
    put16(expected, 0x10 + 104 + 70, 0xff85);
    world.players[1].stocks[4].amount = -9;
    put32(expected, 0x21f6 + 96 + 4 * 8, 0xfffffff7);
    world.stocks[3].float_bits[2] = 0x7fa12345;
    put32(expected, 0x2376 + 3 * 36 + 20, 0x7fa12345);
    auto &offer = world.offers[3][6];
    constexpr std::size_t offer_offset = 0x2526 + 3 * 84 + 6 * 12;
    offer.kind = OfferKind::property;
    expected[offer_offset] = 2;
    offer.flags = 0xde;
    expected[offer_offset + 1] = 0xde;
    offer.item = 4001;
    put16(expected, offer_offset + 2, 4001);
    offer.price = -123456;
    put32(expected, offer_offset + 4, 0xfffe1dc0);
    offer.quantity = 0xfedc;
    put16(expected, offer_offset + 8, 0xfedc);
    offer.property_type = 4;
    expected[offer_offset + 10] = 4;
    offer.property_level = 5;
    expected[offer_offset + 11] = 5;
    world.stock_history_bits[2][143] = 0xffc56789;
    put32(expected, 0x6f6 + 2 * 576 + 143 * 4, 0xffc56789);
    world.public_fund = -1;
    put32(expected, 0x26ba, 0xffffffff);
    world.lease_mode = 4;
    put32(expected, 0x267e, 4);
    world.camera_rotation = 7;
    put32(expected, 0x2743, 7);
    world.round_counter = 0xffffffff;
    put32(expected, 0x2692, 0xffffffff);
    world.month_counter = 0xfedcba98;
    put32(expected, 0x2696, 0xfedcba98);
    world.lottery_tickets[35] = 4;
    expected[0x26be + 35] = 4;
    world.stock_market_index = -32;
    put32(expected, 0x26a2, 0xffffffe0);
    world.stock_market_movement_bits = 0x7fa01234;
    put32(expected, 0x26a6, 0x7fa01234);
    auto &object = world.objects[45];
    const auto object_offset = 0x204 + 45 * 24;
    object.kind = 18;
    expected[object_offset] = 18;
    object.direction = 7;
    expected[object_offset + 1] = 7;
    object.node_id = 4;
    put16(expected, object_offset + 2, 4);
    object.remaining_days = 9;
    expected[object_offset + 4] = 9;
    object.attached_player = 3;
    expected[object_offset + 5] = 3;
    object.animation_remaining = 254;
    expected[object_offset + 6] = 254;
    object.animation_step = 231;
    expected[object_offset + 7] = 231;
    object.motion_bits[3] = 0x7faabcde;
    put32(expected, object_offset + 20, 0x7faabcde);
    world.special_actors[4].inactive_state = 3;
    expected[0x1b4 + 4 * 16 + 10] = 3;
    const auto land = map_offset + u32(input, map_offset + 12) + 52;
    world.lands[1].economy.owner_code = 2;
    expected[land + 25] = 2;
    world.lands[1].economy.level = 5;
    expected[land + 26] = 5;
    world.lands[1].land_price = 3200;
    put16(expected, land + 28, 3200);
    const auto company = map_offset + u32(input, map_offset + 28) + 52;
    world.commercials[1].funds = -1;
    put32(expected, company + 40, 0xffffffff);
    world.commercials[1].shareholder_order = {3, 1, 0, 0};
    std::copy_n(world.commercials[1].shareholder_order.begin(), 4,
                expected.begin() + static_cast<std::ptrdiff_t>(company + 28));
    const auto facility = map_offset + u32(input, map_offset + 20) + 56;
    world.facilities[1].toll_by_level[2] = 321;
    put16(expected, facility + 40, 321);
    world.facilities[1].research_tool = 4;
    expected[facility + 29] = 4;
    world.facilities[1].flags_30 = 9;
    expected[facility + 30] = 9;
    require(world.export_save() == expected,
            "Mutations change exactly their documented bytes, no snapshot or padding drift");
    auto loaded = World::from_save(SaveFile::decode(expected));
    require(loaded.active_player().state.cash == -123 && loaded.lands[1].economy.owner_code == 2 &&
                loaded.facilities[1].research_tool == 4 && loaded.facilities[1].research_days() == 9,
            "Edited runtime state survives original-format reload");
    require(loaded.offers[3][6] == world.offers[3][6], "All twelve listing bytes survive reload");
    world.lands[1].economy.street_name = "new";
    const auto renamed = world.export_save();
    require(renamed[land + 4] == 'n' && renamed[land + 7] == 0 && renamed[land + 19] == 0,
            "Changed names receive bounded NUL padding");
    world.lands[1].economy.street_name = std::string(16, 'x');
    invalid([&] { (void)world.export_save(); });
    world = loaded;
    world.players.pop_back();
    invalid([&] { (void)world.export_save(); });
    world = loaded;
    world.nodes[1].id = 2;
    invalid([&] { (void)world.export_save(); });
    world = loaded;
    world.facilities.push_back({});
    invalid([&] { (void)world.export_save(); });
    world = World::from_save(SaveFile::decode(input));
    auto bankrupt = input;
    std::fill_n(bankrupt.begin() + 0x10 + 104 + 28, 76, 0);
    world.clear_bankrupt_player_fields(1);
    require(world.export_save() == bankrupt && world.players[1].state.cash == 0 &&
                world.players[1].status.cumulative_income == 0 &&
                world.players[1].status.init_cash_ratio == input[0x10 + 104 + 25] &&
                world.players[1].status.stock_investment_ratio == input[0x10 + 104 + 26],
            "Bankruptcy clears the exact original byte range including unknown state but preserves "
            "inventory/profile");
}

void fresh_world() {
    const auto map = synthetic_map();
    NewGameOptions options;
    options.random_seed = 0;
    auto world = World::from_map_resource(map, options);
    require(world.nodes[2].name == "N" && world.nodes[1].name.empty() && world.commercials[1].name == "C" &&
                world.landscapes[1].name == "L",
            "Node/company/landscape names (+4) decode for the map-tile popup");
    require(world.players.size() == 4 && world.header.human_player_count == 1 &&
                world.active_player().state.character == 10,
            "Default new game has chosen human and three opponents");
    Random after_decks(0);
    for (int i = 0; i < 73; ++i)
        (void)after_decks.next();
    require(world.has_initial_random_state() && world.initial_random_state() == after_decks.state(),
            "Only the 36+37 original deck draws precede deferred spawning; seed zero is valid");
    auto sorted_news = world.news_deck;
    auto sorted_fortune = world.fortune_deck;
    std::sort(sorted_news.begin(), sorted_news.end());
    std::sort(sorted_fortune.begin(), sorted_fortune.end());
    for (std::size_t i = 0; i < sorted_news.size(); ++i)
        require(sorted_news[i] == i, "News is a complete permutation");
    for (std::size_t i = 0; i < sorted_fortune.size(); ++i)
        require(sorted_fortune[i] == i, "Fortune is a complete permutation");
    require(world.prison_roster[4] == 1 && world.prison_roster[5] == 1 && world.hospital_roster[6] == 1 &&
                world.hospital_roster[7] == 1,
            "Original special-actor prison/hospital roster defaults");
    std::array<unsigned, 19> object_counts{};
    for (const auto &object : world.objects) {
        require(object.kind > 0 && object.kind <= 18 && object.node_id == 0 && object.attached_player == 0,
                "Fresh objects have exact types and await placement");
        ++object_counts[object.kind];
    }
    for (std::size_t kind = 1; kind <= 14; ++kind)
        require(object_counts[kind] == 1, "Fourteen distinct gods");
    require(object_counts[15] == 2 && object_counts[16] == 10 && object_counts[17] == 10 &&
                object_counts[18] == 10,
            "Original bomb/barrier/mine object pool sizes");
    require(world.players[0].status.loan_ratio == 30 && world.players[0].status.card_strategy == 3 &&
                world.players[0].status.stock_investment_ratio == 15 && world.month_counter == 0 &&
                world.lottery_tickets[35] == 0 && world.camera_rotation == 0 && world.round_counter == 0,
            "Fresh profile and persistent global defaults");
    for (const auto &player : world.players)
        require(player.state.node_id == 0 && player.state.last_node_id == 0,
                "Default placement waits for each player's first turn");
    require(world.players[0].state.cash == 100000 && world.players[1].state.cash == 120000 &&
                world.players[1].state.bank_balance == 80000,
            "Human split and character AI cash ratio");
    require(world.players[0].state.tool_amounts[8] == 1 && world.tool_supply[0] == 6 &&
                world.tool_supply[7] == 6,
            "Original initial tools and shared counts");
    require(world.lands[1].economy.owner_code == 0 && world.lands[1].economy.level == 0,
            "Fresh property state comes from the unowned map template");
    const auto bytes = world.export_save();
    const auto reloaded = World::from_save(SaveFile::decode(bytes));
    require(reloaded.export_save() == bytes && !reloaded.has_initial_random_state(),
            "Fresh state is readable in legacy save layout; old format does not claim saved PRNG state");
    options.players.resize(1);
    options.players[0].start_node = 2;
    world = World::from_map_resource(map, options);
    require(world.players[0].state.node_id == 2 && world.players[0].state.x == 128 &&
                (world.nodes[2].flags & 0x100) != 0 && world.initial_random_state() != 0,
            "Explicit start sets position/occupancy and chooses a real linked previous node");
    for (std::size_t size = 0; size < 40; ++size)
        invalid([&] { (void)World::from_map_resource(std::span(map).first(size)); });
    auto bad = map;
    put32(bad, 4, 0xffffffff);
    invalid([&] { (void)World::from_map_resource(bad); });
    bad = map;
    put16(bad, 40 + 40 + 24, 0xffff);
    invalid([&] { (void)World::from_map_resource(bad); });
    options.players[0].character = 12;
    invalid([&] { (void)World::from_map_resource(map, options); });
}
Bytes read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
void verify_asset(const std::string &kind, const std::string &path) {
    const auto bytes = read_file(path);
    if (kind == "--save") {
        const auto save = SaveFile::decode(bytes);
        const auto world = World::from_save(save);
        require(world.export_save() == bytes, "Real save typed byte roundtrip");
        for (std::size_t slot = 0; slot < world.players.size(); ++slot)
            require(world.players[slot].state.node_id == save.player(slot).node_id,
                    "Real save player/node association");
    } else if (kind == "--map") {
        const auto world = World::from_map_resource(bytes);
        const auto output = world.export_save();
        auto expected_map = bytes;
        for (std::size_t id = 1; id < world.commercials.size(); ++id)
            put32(expected_map, world.map_summary().commercials.byte_offset + id * 52 + 48,
                  static_cast<std::uint32_t>(world.commercials[id].shares_remaining));
        require(std::equal(expected_map.begin(), expected_map.end(), output.begin() + map_offset),
                "Original map records preserved except explicitly initialized commercial remaining shares");
        require(World::from_save(SaveFile::decode(output)).export_save() == output,
                "Original map fresh export/reload");
    } else
        throw std::runtime_error("Usage: world_tests [--save SAVE.DAT] [--map decoded-map.bin] ...");
    std::cout << "Verified " << kind << ' ' << path << '\n';
}
} // namespace

int main(int argc, char **argv) {
    try {
        lossless_and_patching();
        fresh_world();
        if ((argc - 1) % 2 != 0)
            throw std::runtime_error("Asset arguments require option/path pairs");
        for (int i = 1; i < argc; i += 2)
            verify_asset(argv[i], argv[i + 1]);
        std::cout << "World state tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
