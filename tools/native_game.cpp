// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_audio.hpp" // AUDIO: walk loops, context music
#include "native_audio.hpp"
#include "native_frame.hpp"
#include "platform.hpp"
#include "screens/voices.hpp" // AUDIO: auction / lottery announcer clips
#include "native_stock_names.hpp"
#include "native_ui.hpp"
#include "screens/finance.hpp"
#include "screens/main_screen.hpp"
#include "screens/menus.hpp"
#include "rich4/assets.hpp"
#include "screens/items.hpp"
#include "screens/messages.hpp"
#include "rich4/board_renderer.hpp"
#include "rich4/calendar.hpp"
#include "rich4/direction.hpp"
#include "rich4/game.hpp"
#include "rich4/services.hpp"
#include "rich4/stocks.hpp"
#include "rich4/trading.hpp"
#include "rich4/world.hpp"
#include "rich4/world_validation.hpp"
#include "screens/effects.hpp" // EFFECTS: original board animations
#include "screens/intro.hpp"   // new-game airplane / parachute intro
#include "screens/spinners.hpp" // SPINNERS: god slot machine / cupid wheels
#include "screens/facility.hpp" // FACILITY: original facility-type picker (Data 517:4 strip)
#include "screens/monthly.hpp"  // MONTHLY: dividend table, lottery draw, month-end report
#include "screens/endgame.hpp"  // END SCREENS: PLAY AGAIN / stage clear
#include "screens/help.hpp"     // HELP: the original Help window
#include "screens/minigames.hpp" // MINIGAMES: 企鵝挖寶 / 七彩氣球 / 喜從天降
#include "screens/tile_popup.hpp" // map-tile info popup (main-screen.md §7)
#include "screens/ai_settings.hpp" // 託管AI window

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
// Phone builds: same hooks on Android and iOS (fullscreen, touch pad, bundled paths, background autosave).
#ifdef __ANDROID__
#include "rich4_android.hpp" // android/app/src/main/cpp
#define RICH4_MOBILE 1
namespace rich4 { namespace mobile_platform = android_platform; }
#elif defined(TARGET_OS_IOS) && TARGET_OS_IOS
#include "rich4_ios.hpp" // ios/
#define RICH4_MOBILE 1
namespace rich4 { namespace mobile_platform = ios_platform; }
#endif

#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

namespace {
using namespace rich4;
namespace fs = std::filesystem;
namespace messages = native::messages;
namespace spinners = native::spinners;
namespace facility_ui = native::facility; // FACILITY (rich4::Facility is the world record)
namespace minigames = native::minigames;
using native::Artwork;
using native::Button;
using native::map_names;
using native::names;
namespace screens = native::screens;

void require(bool success) {
    if (!success)
        throw std::runtime_error(SDL_GetError());
}
std::string quoted(std::string_view text) {
    std::string result = "\"";
    for (const auto byte : text) {
        if (byte == '\"' || byte == '\\') {
            result += '\\';
            result += byte;
        } else if (byte == '\n')
            result += "\\n";
        else if (static_cast<unsigned char>(byte) >= 32)
            result += byte;
    }
    return result + '"';
}

struct Options {
    fs::path assets, font, load, save_directory, output;
    bool headless = false, start_new = false, hidden_window = false, exercise_recovery = false;
    bool exercise_trading = false, overlay_gallery = false;
    bool play_effects = false; // EFFECTS (dev): play effects in replays and dump effect-NNN.ppm frames
    bool touch = false; // finger-sized number pad (always on Android; --touch on desktop)
    fs::path gallery;   // dev: render every original-style menu/finance screen to DIR and exit
    bool items_gallery = false; // dev: render every card/tool/shop screen into OUTPUT/gallery
    bool colour_gallery = false; // dev: info panel + owned plots for all 12 characters (OUTPUT/colours)
    bool monthly_gallery = false; // MONTHLY (dev): play sample monthly screens into OUTPUT/gallery
    std::string ui_preset;          // dev: initial main-screen state, e.g. "view=2,tab=1,calendar=1,map=1"
    unsigned capture_every = 0;     // dev: also dump every Nth rendered frame into --output
    bool start_loaded = false;      // dev: open --load straight into play (no menu) in real time
    std::optional<SaveDate> start_date; // --date YYYY-MM-DD for new games (replays default to 2026-10-05)
    int minigame = -1;              // MINIGAMES (dev): --minigame penguin|balloon|catch plays one directly
    unsigned auto_rounds = 0;
    unsigned map = 0;
    unsigned exercise_card = 0, exercise_tool = 0;
    std::uint32_t seed = static_cast<std::uint32_t>(std::time(nullptr));
};

Options arguments(int argc, char **argv) {
    const fs::path home = std::getenv("HOME") ? std::getenv("HOME") : ".";
    Options options;
    options.assets = RICH4_DEFAULT_ASSETS;
    options.save_directory = home / "Library/Application Support/Rich4Native/saves";
#ifdef RICH4_MOBILE
    // Android: extracted APK assets; iOS: the app bundle. Bundled/system font, SDL pref path.
    const auto platform = mobile_platform::prepare();
    options.assets = platform.assets;
    options.font = platform.font;
    options.save_directory = platform.save_directory;
    options.touch = true;
#endif
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--headless")
            options.headless = true;
        else if (argument == "--hidden-window")
            options.hidden_window = true;
        else if (argument == "--start-loaded")
            options.start_loaded = true;
        else if (argument == "--exercise-recovery")
            options.exercise_recovery = true;
        else if (argument == "--exercise-trading")
            options.exercise_trading = true;
        else if (argument == "--play-effects")
            options.play_effects = true;
        else if (argument == "--overlay-gallery") // Dev: dump sample bubbles/boxes/news as gallery-*.ppm
            options.overlay_gallery = true;
        else if (argument == "--colour-gallery")
            options.colour_gallery = true;
        else if (argument == "--items-gallery")
            options.items_gallery = true;
        else if (argument == "--monthly-gallery") // MONTHLY (dev)
            options.monthly_gallery = true;
        else if (argument == "--start-new")
            options.start_new = true;
        else if (argument == "--touch")
            options.touch = true;
        else {
            if (index + 1 >= argc)
                throw std::invalid_argument("Missing value for " + argument);
            const std::string value = argv[++index];
            if (argument == "--assets")
                options.assets = value;
            else if (argument == "--font")
                options.font = value;
            else if (argument == "--load")
                options.load = value;
            else if (argument == "--save-dir")
                options.save_directory = value;
            else if (argument == "--output")
                options.output = value;
            else if (argument == "--auto-rounds")
                options.auto_rounds = static_cast<unsigned>(std::stoul(value));
            else if (argument == "--seed")
                options.seed = static_cast<std::uint32_t>(std::stoul(value));
            else if (argument == "--date") { // YYYY-MM-DD
                const auto year = std::stoi(value.substr(0, 4)), month = std::stoi(value.substr(5, 2)),
                           day = std::stoi(value.substr(8, 2));
                options.start_date = SaveDate{static_cast<std::uint8_t>(day), static_cast<std::uint8_t>(month),
                                              static_cast<std::uint16_t>(year)};
            }
            else if (argument == "--map")
                options.map = static_cast<unsigned>(std::stoul(value));
            else if (argument == "--exercise-card")
                options.exercise_card = static_cast<unsigned>(std::stoul(value));
            else if (argument == "--ui-preset")
                options.ui_preset = value;
            else if (argument == "--capture-every")
                options.capture_every = static_cast<unsigned>(std::stoul(value));
            else if (argument == "--exercise-tool")
                options.exercise_tool = static_cast<unsigned>(std::stoul(value));
            else if (argument == "--gallery")
                options.gallery = value;
            else if (argument == "--minigame") // MINIGAMES (dev)
                options.minigame = value == "penguin" ? 0 : value == "balloon" ? 1 : value == "catch" ? 2 : -1;
            else
                throw std::invalid_argument("Unknown argument: " + argument);
        }
    }
    if (options.headless && !options.auto_rounds)
        options.auto_rounds = 3;
    if (options.map > 7)
        throw std::invalid_argument("Map must be 0..7");
    if (options.exercise_card > 30 || options.exercise_tool > 13)
        throw std::invalid_argument("Invalid requested interaction ID");
    if (!options.gallery.empty()) {
        options.headless = true;
        options.gallery = fs::absolute(options.gallery);
    }
    if (options.headless && options.output.empty() && options.gallery.empty())
        throw std::invalid_argument("Headless testing requires --output DIR");
    if (options.font.empty()) {
        const std::array candidates{
            fs::path("/Applications/Microsoft Word.app/Contents/Resources/DFonts/mingliu.ttc"),
            home / "Library/Containers/org.boxedwine/Data/Library/Application "
                   "Support/Boxedwine/Rich4/root/home/username/.wine/drive_c/windows/Fonts/mingliu.ttc",
            fs::path("/System/Library/Fonts/PingFang.ttc"),
            fs::path("/System/Library/Fonts/STHeiti Medium.ttc")};
        for (const auto &path : candidates)
            if (fs::is_regular_file(path)) {
                options.font = path;
                break;
            }
    }
    if (options.font.empty())
        throw std::runtime_error("No Chinese font found; specify --font FONT.ttf");
    options.save_directory = fs::absolute(options.save_directory);
    if (!options.output.empty())
        options.output = fs::absolute(options.output);
    return options;
}

// Append only: the values are logged in inputs.jsonl and name the screen-N.ppm captures.
enum class Screen {
    menu,
    new_game,
    game,
    settings,
    wealth,
    cards,
    tools,
    stocks,
    trading,
    load,
    save,
    ai_settings
};

class Application {
  public:
    explicit Application(Options options)
        : options_(std::move(options)), artwork_(options_.assets), font_(options_.font) {
        selected_map_ = options_.map % 4;
        selected_stage_ = options_.map / 4;
        if (options_.headless) {
            require(SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE));
            require(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE));
        }
        if (options_.hidden_window)
            require(SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1"));
        require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
        window_.reset(
            SDL_CreateWindow("大富翁 4", 960, 720,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                 ((options_.headless || options_.hidden_window) ? SDL_WINDOW_HIDDEN : 0)));
        if (!window_)
            throw std::runtime_error(SDL_GetError());
#ifdef RICH4_MOBILE
        SDL_SetWindowFullscreen(window_.get(), true); // immersive: hide status and navigation bars
        require(SDL_AddEventWatch(save_on_background, this));
#endif
        renderer_.reset(SDL_CreateRenderer(window_.get(), options_.headless ? "software" : nullptr));
        if (!renderer_)
            throw std::runtime_error(SDL_GetError());
        if (!options_.headless)
            SDL_SetRenderVSync(renderer_.get(), 1);
        require(
            SDL_SetRenderLogicalPresentation(renderer_.get(), 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX));
        texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, 640, 480));
        if (!texture_)
            throw std::runtime_error(SDL_GetError());
        require(SDL_SetTextureScaleMode(texture_.get(), SDL_SCALEMODE_NEAREST));
        if (options_.headless && std::string(SDL_GetCurrentVideoDriver()) != "dummy")
            throw std::runtime_error("Native headless verification must use the dummy video driver");
        if (!options_.output.empty()) {
            fs::create_directories(options_.output);
            events_.open(options_.output / "events.jsonl");
            inputs_.open(options_.output / "inputs.jsonl");
        }
        audio_ = std::make_unique<audio::Audio>(options_.assets);
        pad_.set_scale(options_.touch ? 2 : 1);
        effects_.set_skip(options_.auto_rounds && !options_.play_effects); // EFFECTS: replays stay zero-duration
        spinners_.set_skip(options_.auto_rounds && !options_.play_effects); // SPINNERS: same as effects
        monthly_.set_skip(options_.auto_rounds && !options_.play_effects); // MONTHLY: one frame per screen
        messages_.set_instant(options_.auto_rounds != 0); // Headless replay never waits on a bubble.
        main_screen_.set_skip_toss(options_.auto_rounds && !options_.play_effects); // replays stay fast
        items_.sound = [this](std::size_t effect) {
            if (sound_effects_)
                audio_->play_sound(effect);
        };
        items_.speech = [this](std::size_t clip, bool queued) { speak(clip, queued); }; // AUDIO
        if (!options_.output.empty()) { // AUDIO: which music/loops/voices start and stop, in order
            audio_log_.open(options_.output / "audio.jsonl");
            audio_->set_trace([this](std::string_view what, std::string_view detail) {
                ++audio_log_counts_[std::string(what)];
                audio_log_ << "{\"frame\":" << rendered_frames_ << ",\"day\":"
                           << (session_ ? session_->completed_days() : 0) << ",\"audio\":" << quoted(what)
                           << ",\"detail\":" << quoted(detail) << "}\n";
            });
        }
        if (!audio_->music_available())
            std::cerr << audio_->music_diagnostic() << '\n';
    }
    ~Application() {
        audio_.reset();
        texture_.reset();
        renderer_.reset();
        window_.reset();
        SDL_Quit();
    }

    void write_diagnostic(std::string_view reason) const {
        if (options_.output.empty() || !session_)
            return;
        ui::write_file(options_.output / "failure-world.DAT", session_->world().export_save());
        std::ofstream output(options_.output / "failure.json");
        output << "{\"resumable_save\":false,\"reason\":" << quoted(reason)
               << ",\"phase\":" << int(session_->phase()) << ",\"random_state\":" << session_->random_state()
               << ",\"completed_days\":" << session_->completed_days() << ",\"decision\":"
               << quoted(session_->pending_decision() ? session_->pending_decision()->prompt : "") << "}\n";
    }

    int run() {
        if (options_.items_gallery) {
            if (options_.output.empty())
                throw std::invalid_argument("--items-gallery requires --output DIR");
            screens::NewGameChoice choice; // 小丹尼 (human) and three computer opponents
            choice.players = {{10, 1}, {0, 2}, {1, 2}, {2, 2}};
            new_game(choice);
            fs::create_directories(options_.output / "gallery");
            items_.gallery(
                frame_, session_->world(),
                [this] {
                    buttons_.clear();
                    canvas_ = graphics::make_canvas(640, 480, ui::color(7, 30, 55));
                    draw_board();
                },
                [this](const std::string &name) {
                    graphics::write_ppm(canvas_, options_.output / "gallery" / (name + ".ppm"));
                });
            return 0;
        }
        if (options_.colour_gallery) {
            if (options_.output.empty())
                throw std::invalid_argument("--colour-gallery requires --output DIR");
            colour_gallery();
            return 0;
        }
        if (options_.monthly_gallery && !options_.output.empty()) { // MONTHLY (dev): sample screens
            screens::NewGameChoice choice;
            choice.players = {{10, 1}, {0, 2}, {5, 2}, {9, 2}};
            new_game(choice);
            fs::create_directories(options_.output / "gallery");
            monthly_.gallery(
                frame_, session_->world(), [this] { canvas_ = graphics::make_canvas(640, 480, 0); },
                [this](const std::string &name) {
                    graphics::write_ppm(canvas_, options_.output / "gallery" / (name + ".ppm"));
                });
            return 0;
        }
        if (!options_.gallery.empty())
            return gallery();
        if (options_.start_loaded && !options_.load.empty() && !options_.auto_rounds)
            load(options_.load);
        auto last_tick = SDL_GetTicks();
        for (unsigned iteration = 0; running_; ++iteration) {
            draw();
            // Replay input waits for movies; they only play on the board, so another screen (e.g. the
            // stock market opened at a turn start) must still be driven, or a queued movie deadlocks.
            if (options_.auto_rounds && !(effects_.active() && screen_ == Screen::game) && !intro_.active() &&
                options_.minigame < 0) // MINIGAMES dev runs alone
                drive_test();
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (intro_.active() && screen_ == Screen::game) {
                    // The intro movies are skippable by mouse-up or key-up (fcn_0045144f flags bit 1);
                    // a skip ends the whole intro. Tap and Android Back skip too. Nothing else
                    // reacts to input until it ends. Only a release whose press happened during the
                    // intro counts: the release of the setup's OK press must not skip it at once.
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat))
                        intro_pressed_ = true;
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
                        frame_.pointer_down = false; // no stale "pressed" control after the intro
                    if ((event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_KEY_UP) &&
                        intro_pressed_) {
                        intro_.skip();
                        continue;
                    }
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                        event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_MOUSE_MOTION ||
                        event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT ||
                        event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_UP ||
                        event.type == SDL_EVENT_FINGER_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL)
                        continue;
                }
                if (event.type == SDL_EVENT_QUIT) {
                    if (screen_ == Screen::stocks)
                        close_stocks();
                    else if (screen_ == Screen::trading)
                        close_trading();
                    running_ = false;
                    continue;
                }
                if (event.type == SDL_EVENT_TEXT_INPUT && editing_amount_) {
                    for (const char *ch = event.text.text; *ch; ++ch)
                        if (*ch >= '0' && *ch <= '9' && amount_text_.size() < 10)
                            amount_text_ += *ch;
                    update_amount();
                }
                if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    require(SDL_ConvertEventToRenderCoordinates(renderer_.get(), &event));
                    const bool motion = event.type == SDL_EVENT_MOUSE_MOTION;
                    frame_.pointer_x = int(motion ? event.motion.x : event.button.x);
                    frame_.pointer_y = int(motion ? event.motion.y : event.button.y);
                    if (!motion)
                        frame_.pointer_down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    if (screen_ == Screen::game && session_) {
                        if (motion)
                            main_screen_.pointer_motion(frame_.pointer_x, frame_.pointer_y,
                                                        (event.motion.state & SDL_BUTTON_LMASK) != 0);
                        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
                            main_screen_.pointer_up(frame_.pointer_x, frame_.pointer_y,
                                                    event.button.button == SDL_BUTTON_RIGHT);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT &&
                    cancellable()) {
                    buttons_.clear();
                    cancel_screen(); // The original's right-button-up means cancel/back on these screens.
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT &&
                    screen_ == Screen::game && session_ && !messages_.active() && session_->pending_decision() &&
                    messages::is_yes_no(*session_->pending_decision())) {
                    buttons_.clear();
                    respond(yes_no_index(0)); // Right-click answers NO, like _rich4_ui_yesno.
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                    const int x = int(event.button.x), y = int(event.button.y);
                    for (const auto &button : buttons_)
                        if (button.enabled && ui::contains(button.rectangle, x, y)) {
                            if (button.id != native::tile_popup::hit_id)
                                tile_popup_.reset(); // a tap elsewhere dismisses the tile popup
                            auto action = button.action;
                            ++input_events_;
                            if (inputs_)
                                inputs_ << "{\"button\":" << button.id << ",\"screen\":" << int(screen_)
                                        << ",\"day\":" << (session_ ? session_->completed_days() : 0)
                                        << "}\n";
                            if (sound_effects_ && button.click_sound && button.id != messages::skip_button)
                                audio_->play_sound(9);
                            if (button.id != messages::skip_button && !options_.auto_rounds)
                                platform::haptic_tap(); // a light tap on phones for every button press
                            buttons_.clear(); // Remaining events cannot invoke controls from the previous
                                              // screen/decision.
                            action();
                            break;
                        }
                }
#ifdef __ANDROID__
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_AC_BACK)
                    event.key.key = SDLK_ESCAPE; // system Back behaves like Escape
#endif
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && screen_ == Screen::game &&
                    messages_.active() && !editing_amount_) {
                    buttons_.clear();
                    messages_.skip(); // Any key skips a bubble/box/news, as in the original.
                    continue;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    buttons_.clear();
                    if (pad_.active()) {
                        pad_key(event.key.key);
                        continue;
                    }
                    if (event.key.key == SDLK_ESCAPE && cancellable()) {
                        cancel_screen();
                        continue;
                    }
                    if (editing_amount_) {
                        if (event.key.key == SDLK_BACKSPACE) {
                            if (!amount_text_.empty())
                                amount_text_.pop_back();
                            update_amount();
                        }
                        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_ESCAPE)
                            end_amount_edit();
                        continue;
                    }
                    if (event.key.key == SDLK_ESCAPE) {
                        if (screen_ == Screen::stocks)
                            close_stocks();
                        else if (screen_ == Screen::trading)
                            close_trading();
                        else
                            screen_ = screen_ == Screen::game ? Screen::menu
                                      : session_              ? Screen::game
                                                              : Screen::menu;
                    }
                    if ((event.key.key == SDLK_SPACE || event.key.key == SDLK_RETURN) &&
                        screen_ == Screen::game && session_) {
                        if (session_->phase() == GamePhase::human_ready && presentation_.empty() && !presenting())
                            session_->roll_dice(); // only once the previous turns have been shown
                        else if (session_->pending_decision() && presentation_.empty() &&
                                 !presenting()) { // never answer a decision that is not shown yet
                            if (map_decision()) {
                                if (selected_target_)
                                    respond(*selected_target_);
                            } else
                                respond(0);
                        }
                    }
                    if (event.key.key == SDLK_LEFT && screen_ == Screen::game)
                        rotate(7);
                    if (event.key.key == SDLK_RIGHT && screen_ == Screen::game)
                        rotate(1);
                    if (event.key.key == SDLK_V && screen_ == Screen::game)
                        main_screen_.cycle_view_mode();
                    if (event.key.key == SDLK_F5 && session_ && session_->can_save())
                        save();
                    if (event.key.key == SDLK_F9 && fs::exists(load_path()))
                        load(load_path());
                }
            }
            const auto now = SDL_GetTicks();
            const unsigned elapsed =
                options_.auto_rounds
                    ? (options_.play_effects && session_ && // dev: real-time toss/dice/spinner frame strips
                               (main_screen_.dice_busy() || session_->phase() == GamePhase::rolling ||
                                (session_->pending_decision() && spinners::handles(*session_->pending_decision())))
                           ? 40
                           : 500)
                                     : static_cast<unsigned>(std::min<Uint64>(now - last_tick, 100)) * speed_;
            last_tick = now;
            if (session_ && screen_ == Screen::game && !items_.popup_active() &&
                !help_.active()) { // the card popup and the Help window are modal
                observe(); // a GO press emits the roll before the clock advances
                hold_ms_ -= std::min(hold_ms_, elapsed); // game time, so it follows the speed setting
                if (!effects_.active() && !messages_.active() && !intro_.active()) // only the step shown advances
                    main_screen_.advance(elapsed);
                pump_presentation();
                // The original roll animation, board effects (EFFECTS) and every bubble/box/news popup
                // block the game loop until everything already emitted has been shown, in order.
                if (presentation_.empty() && !presenting()) {
                    // EFFECTS: the board as it is right before this tick's mutations (commit frames).
                    effects_.remember(session_->world(), session_.get(), SDL_GetTicks());
                    session_->tick(elapsed);
                }
                observe();
                if (session_->can_save() && last_autosaved_day_ != session_->completed_days()) {
                    write_save(options_.save_directory / "Autosave.DAT");
                    last_autosaved_day_ = session_->completed_days();
                }
            }
            if (options_.auto_rounds && iteration > 50000)
                throw std::runtime_error("Native UI replay exceeded its operation limit");
            update_board_audio(); // AUDIO
            audio_->update();
            if (!options_.auto_rounds)
                SDL_Delay(8);
        }
        // Quitting writes the AUTO slot: the manual SAVE1..5 slots only change when the player saves.
        if (session_ && session_->can_save() && !options_.auto_rounds)
            write_save(slot_path(0));
        return final_status_;
    }

  private:
#ifdef RICH4_MOBILE
    // Android and iOS may kill a backgrounded app without SDL_EVENT_QUIT, and SDL blocks the main
    // loop while paused on Android, so lifecycle events must be handled here (SDL calls watches synchronously
    // on the main thread from the event pump).
    static bool SDLCALL save_on_background(void *self, SDL_Event *event) {
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_TERMINATING) {
            try {
                auto &application = *static_cast<Application *>(self);
                if (application.session_ && application.session_->can_save()) // never overwrite manual Save1
                    application.write_save(application.options_.save_directory / "Autosave.DAT");
            } catch (const std::exception &error) { // must not unwind through SDL's C code
                std::cerr << "Background save failed: " << error.what() << '\n';
            }
        }
        return true;
    }
#endif
    void picture(const graphics::Image &image, int x, int y) {
        frame_.picture(image, x, y);
    }
    void button(int id, graphics::Rect rect, std::string_view label, std::function<void()> action,
                bool enabled = true) {
        frame_.button(id, rect, label, std::move(action), enabled);
    }
    void hit(int id, graphics::Rect rect, std::function<void()> action, bool enabled = true) {
        frame_.hit(id, rect, std::move(action), enabled);
    }
    void dialog(graphics::Rect rect, std::string_view title) {
        frame_.dialog(rect, title);
    }
    // ---- Original-style menu and finance screens (tools/screens) ----
    screens::Host make_host() {
        return {frame_,
                [this](int effect) {
                    if (sound_effects_ && effect >= 0)
                        audio_->play_sound(std::size_t(effect));
                },
                [this](std::string_view message) {
                    toast_ = message;
                    toast_until_ = SDL_GetTicks() + 2200;
                },
                options_.auto_rounds != 0 || !options_.gallery.empty(),
                [this](std::size_t clip) { speak(clip, false); }}; // AUDIO: clerk voices
    }
    // AUDIO: a clerk/announcer clip (Speaking.mkf), queued after the current voice if asked.
    void speak(std::size_t clip, bool queued) {
        if (!sound_effects_)
            return;
        if (queued)
            audio_->queue_speech(clip);
        else
            audio_->play_speech(clip);
    }
    // AUDIO: once per frame. Walk/time-bomb loops follow the step being shown; context music
    // follows the screen (menu, setup, intro, decision screens, bankruptcy, victory, holidays).
    void update_board_audio() {
        const auto now = SDL_GetTicks();
        std::optional<std::size_t> walk, bomb;
        if (session_ && screen_ == Screen::game && sound_effects_ && !intro_.active())
            if (const auto &animation = session_->animation()) {
                walk = native::walk_loop_clip(session_->world(), animation->player);
                bomb = native::bomb_loop_clip(session_->world(), animation->player);
            }
        walk_sound_.update(*audio_, walk, bomb, now);
        if (!session_ || !session_->pending_decision())
            voiced_decision_key_.clear(); // the next visit's identical decision speaks again
        if (bankrupt_music_ && presentation_.empty() && !presenting())
            bankrupt_music_ = false;
        std::vector<audio::Music> wanted;
        const bool from_menu = (screen_ == Screen::settings || screen_ == Screen::load) && previous_screen_ == Screen::menu;
        if (screen_ == Screen::menu || from_menu || (!session_ && screen_ != Screen::new_game))
            wanted = {audio::Music::menu}; // ui_main.asm:483
        else if (screen_ == Screen::new_game)
            wanted = {audio::Music::setup}; // new_game.asm:4009 (replaces MIDI01)
        else if (session_) {
            if (const auto holiday = native::holiday_music(session_->world()))
                wanted.push_back(*holiday);
            if (intro_.active() && screen_ == Screen::game)
                wanted = {audio::Music::silence}; // rich4.asm:7236: the intro stops the music
            else if (session_->phase() == GamePhase::finished)
                wanted.push_back(audio::Music::shop); // victory sequence MIDI07, new_game.asm:4531
            else if (bankrupt_music_)
                wanted.push_back(audio::Music::bankrupt); // bankrupt.asm:216
            else if (const std::string_view monthly = monthly_.shot(); monthly == "lottery")
                wanted.push_back(audio::Music::lottery_draw); // MIDI09, fcn_004549cf(8)
            else if (monthly == "report")
                wanted.push_back(audio::Music::bank_report); // MIDI10, fcn_004549cf(9)
            else if (const auto &decision = session_->pending_decision();
                     decision && decision->mini_game && minigames_.active()) {
                static constexpr std::array games{audio::Music::penguin_game, audio::Music::balloon_game,
                                                  audio::Music::gift_game}; // MIDI13 / 12 / 11
                wanted.push_back(games.at(std::min<std::size_t>(decision->mini_game->game, 2)));
            }
            else if (const auto &decision = session_->pending_decision();
                     decision && presentation_.empty() && !effects_.active())
                if (const auto music = native::decision_music(*decision))
                    wanted.push_back(*music);
        }
        music_director_.update(*audio_, wanted, now);
    }
    // AUDIO: announcer lines and sounds of the lottery kiosk and the auction room as their
    // decision first shows (ui_letou.asm bar entry 11-13 / 15, ui_auction.asm 131-132).
    void voice_decision_shown(const GameDecision &decision) {
        if (decision.kind == DecisionKind::lottery) {
            const bool can_buy = std::any_of(decision.options.begin(), decision.options.end(),
                                             [](const auto &option) { return option.value > 0; });
            if (can_buy) {
                speak(11, false);
                speak(12, true);
                speak(13, true);
            } else
                speak(15, false);
        } else if (decision.kind == DecisionKind::auction && !auction_open_) {
            auction_open_ = true;
            speak(131, false);
            speak(132, true);
        }
    }
    void pad_key(SDL_Keycode key) {
        auto host = make_host();
        if (key == SDLK_ESCAPE)
            pad_.cancel(host);
        else if (key >= SDLK_0 && key <= SDLK_9)
            pad_.key(host, char('0' + (key - SDLK_0)));
        else if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
            pad_.key(host, char('1' + (key - SDLK_KP_1)));
        else if (key == SDLK_KP_0)
            pad_.key(host, '0');
        else if (key == SDLK_BACKSPACE)
            pad_.key(host, '\b');
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
            pad_.key(host, '\r');
        else if (key == SDLK_M || key == SDLK_C || key == SDLK_H)
            pad_.key(host, key == SDLK_M ? 'M' : key == SDLK_C ? 'C' : 'H');
    }
    bool facility_cancellable() const { // FACILITY
        return session_ && session_->pending_decision() && facility_ui::handles(*session_->pending_decision()) &&
               facility_ui::Picker::cancellable(*session_->pending_decision());
    }
    bool bank_pending() const {
        return session_ && session_->pending_decision() &&
               session_->pending_decision()->kind == DecisionKind::bank;
    }
    // Screens where right-click / Escape / Android Back mean "cancel" in the original.
    bool cancellable() const {
        return help_.active() || pad_.active() || screen_ == Screen::new_game || screen_ == Screen::settings ||
               screen_ == Screen::wealth || screen_ == Screen::stocks || screen_ == Screen::load ||
               screen_ == Screen::trading || screen_ == Screen::ai_settings ||
               screen_ == Screen::save || (screen_ == Screen::game && (bank_pending() || facility_cancellable()));
    }
    void cancel_screen() {
        auto host = make_host();
        if (help_.active()) { // HELP: right-click / Back / Escape close it
            help_.cancel(host);
            return;
        }
        if (pad_.active()) {
            pad_.cancel(host);
            return;
        }
        switch (screen_) {
        case Screen::stocks:
            stock_market_.cancel(host, stock_actions());
            break;
        case Screen::trading:
            trading_board_.cancel(host, trading_actions());
            break;
        case Screen::ai_settings:
            ai_settings_.cancel(host, ai_settings_actions());
            break;
        case Screen::new_game:
            setup_.cancel(host, [this] { screen_ = Screen::menu; });
            break;
        case Screen::settings:
        case Screen::load:
        case Screen::save:
        case Screen::wealth:
            host.sound(screens::cancel_sound);
            screen_ = back_screen();
            break;
        case Screen::game:
            if (bank_pending())
                bank_screen_.cancel(host, *session_->pending_decision(), bank_actions());
            else if (facility_cancellable()) // FACILITY: Back = the 改建卡 picker's right-click
                facility_picker_.cancel(host, *session_->pending_decision(), [this](std::size_t i) { respond(i); });
            break;
        default:
            break;
        }
    }
    // Where settings / save / load / player info return to: the menu they came from, else the game.
    Screen back_screen() const {
        return session_ && session_->phase() != GamePhase::finished && previous_screen_ != Screen::menu
                   ? Screen::game
                   : Screen::menu;
    }
    // Answers the pending decision with the option whose value is `value`.
    bool respond_value(std::int32_t value, std::int32_t amount) {
        const auto &decision = session_->pending_decision();
        if (!decision)
            return false;
        for (std::size_t index = 0; index < decision->options.size(); ++index)
            if (decision->options[index].value == value) {
                bank_amount_ = amount;
                const bool success = session_->respond(index, amount);
                observe();
                return success;
            }
        return false;
    }
    screens::BankActions bank_actions() {
        return {[this](std::int32_t value, std::int32_t amount) { return respond_value(value, amount); }};
    }
    screens::StockActions stock_actions() {
        return {[this](std::size_t stock, std::int32_t quantity) {
                    selected_stock_ = stock;
                    return trade(true, quantity);
                },
                [this](std::size_t stock, std::int32_t quantity) {
                    selected_stock_ = stock;
                    return trade(false, quantity);
                },
                [this] { close_stocks(); }};
    }
    // Save slots: 0 is the automatic save (load only, shown as AUTO), 1..5 are manual.
    fs::path slot_path(int slot) const {
        return options_.save_directory / (slot == 0 ? std::string("Autosave.DAT") : "Save" + std::to_string(slot) + ".DAT");
    }
    std::array<screens::SaveSlot, 6> read_slots() const {
        std::array<screens::SaveSlot, 6> slots{};
        for (int slot = 0; slot < 6; ++slot) {
            try {
                if (!fs::exists(slot_path(slot)))
                    continue;
                const auto save = SaveFile::decode(ui::read_file(slot_path(slot)));
                const auto header = save.header();
                if (header.signature != 0x26)
                    continue;
                auto &entry = slots[std::size_t(slot)];
                entry.valid = true;
                entry.date = header.date;
                entry.map = header.map_id;
                entry.stage = header.stage_id;
                for (std::size_t player = 0; player < std::min<std::uint32_t>(header.player_count, 4); ++player)
                    entry.characters.push_back(save.player(player).character);
            } catch (const std::exception &) {
            }
        }
        return slots;
    }
    bool any_slot() const {
        for (int slot = 0; slot < 6; ++slot)
            if (fs::exists(slot_path(slot)))
                return true;
        return false;
    }
    void open_load() {
        if (!options_.load.empty()) { // An imported DAT (or the replay's --load) loads directly.
            load(options_.load);
            return;
        }
        const auto slots = read_slots();
        if (std::none_of(slots.begin(), slots.end(), [](const auto &slot) { return slot.valid; })) {
            toast_ = "沒有存檔";
            toast_until_ = SDL_GetTicks() + 2200;
            return;
        }
        slots_.open(false, slots);
        screen_ = Screen::load;
    }
    void open_save() {
        if (!session_ || !session_->can_save())
            return;
        slots_.open(true, read_slots());
        screen_ = Screen::save;
    }
    void choose_slot(int slot) {
        if (slots_.saving()) {
            write_save(slot_path(slot));
            options_.load.clear();
            ++saves_;
            toast_ = "存檔完成";
            toast_until_ = SDL_GetTicks() + 2200;
            screen_ = Screen::game;
            return;
        }
        const auto previous = back_screen();
        if (!load(slot_path(slot)))
            screen_ = previous;
    }
    void notify_action(bool success, std::string_view failure = "目前無法使用") {
        if (!success) {
            toast_ = failure;
            toast_until_ = SDL_GetTicks() + 2200;
        }
        if (session_)
            observe();
    }
    void update_amount() {
        if (!editing_amount_)
            return;
        const auto value = amount_text_.empty() ? 0LL : std::stoll(amount_text_);
        *editing_amount_ = static_cast<std::int32_t>(std::clamp<long long>(value, 0, amount_limit_));
    }
    void end_amount_edit() {
        editing_amount_ = nullptr;
        SDL_StopTextInput(window_.get());
    }
    fs::path save_path() const {
        return options_.save_directory / "Save1.DAT";
    }
    fs::path load_path() const {
        if (!options_.load.empty())
            return options_.load;
        // Continue from whichever slot is newest: Android backgrounding writes only the autosave.
        auto newest = save_path();
        for (int slot = 0; slot <= 5; ++slot) {
            const auto candidate = slot_path(slot);
            if (fs::exists(candidate) &&
                (!fs::exists(newest) || fs::last_write_time(candidate) > fs::last_write_time(newest)))
                newest = candidate;
        }
        return newest;
    }
    void rotate(unsigned increment) {
        rotation_ = (rotation_ + increment) % 8;
        if (session_)
            session_->set_camera_rotation(static_cast<std::uint8_t>(rotation_));
    }
    void open_setup(unsigned stage) {
        setup_.open(stage, stage == selected_stage_ ? selected_map_ : 0, options_.seed);
        screen_ = Screen::new_game;
    }
    // Picked characters are humans in click order, the auto-filled rest are computers
    // (rich4_new_game.asm). The original picks computers with the gameplay rand(); the
    // native setup uses its own generator seeded from --seed instead.
    void new_game(const screens::NewGameChoice &choice) {
        options_.load.clear();
        NewGameOptions configuration;
        const auto time = std::time(nullptr);
        const auto local = *std::localtime(&time);
        configuration.date = {static_cast<std::uint8_t>(local.tm_mday),
                              static_cast<std::uint8_t>(local.tm_mon + 1),
                              static_cast<std::uint16_t>(local.tm_year + 1900)};
        // Holidays, monthly events and card coverage depend on the start date, so automated runs use
        // a fixed one (--date, default 2026-10-05) to stay reproducible from day to day.
        if (options_.start_date)
            configuration.date = *options_.start_date;
        else if (options_.auto_rounds)
            configuration.date = {5, 10, 2026};
        selected_map_ = choice.map;
        selected_stage_ = choice.stage;
        audio_->select_music(1); // AUDIO: a new game starts the board playlist at RICH08 (rich4.asm:7259)
        music_director_.forget();
        configuration.map_id = static_cast<std::uint16_t>(choice.map);
        configuration.stage_id = static_cast<std::uint16_t>(choice.stage);
        configuration.initial_fund = choice.fund;
        configuration.lease_mode = choice.lease_mode;
        configuration.random_seed = options_.seed;
        configuration.players = choice.players;
        try {
            install_session(World::from_map_resource(
                                artwork_.archive('m').decode((choice.stage * 4 + choice.map) * 2 + 1), configuration),
                            options_.seed, true);
        } catch (const std::exception &error) {
            std::cerr << "New game failed: " << error.what() << '\n';
            toast_ = "無法開始新遊戲";
            toast_until_ = SDL_GetTicks() + 3500;
            screen_ = Screen::menu;
            return;
        }
        ++new_games_;
    }
    // intro: a new game plays the original airplane/parachute intro before the first turn
    // (rich4_main.asm:376 fcn_00415872); loading a save does not (loc_00401d08).
    // Options 結束遊戲 during a game: keep the progress in the AUTO slot (when the turn can be saved),
    // close the session and return to the title screen. Only the title screen's EXIT quits the app.
    void end_to_title() {
        if (session_ && session_->can_save() && !options_.auto_rounds)
            write_save(options_.save_directory / "Autosave.DAT");
        messages_.clear();
        spinners_.clear();
        monthly_.clear();
        minigames_.clear();
        effects_.clear();
        presentation_.clear();
        hold_ms_ = 0;
        presented_player_.reset();
        items_.reset();
        auction_room_.reset();
        walk_sound_.stop(*audio_);
        tile_popup_.reset();
        session_.reset();
        screen_ = previous_screen_ = Screen::menu;
    }
    void install_session(World world, std::uint32_t seed, bool intro = false) {
        validate_playable_world(world);
        end_amount_edit();
        const auto map = static_cast<std::uint8_t>(world.header.stage_id * 4 + world.header.map_id);
        auto session = std::make_unique<GameSession>(std::move(world), seed);
        auto board = std::make_unique<board::Renderer>(options_.assets, map);
        session_ = std::move(session);
        board_ = std::move(board);
        if (!initial_random_state_)
            initial_random_state_ = session_->random_state();
        rotation_ = session_->world().camera_rotation % 8;
        screen_ = Screen::game;
        endgame_.close(); // END SCREENS
        endgame_session_ = nullptr;
        humans_at_start_ = 0;
        for (const auto &player : session_->world().players)
            humans_at_start_ += (player.state.controller_flags & 1U) != 0;
        last_autosaved_day_ = UINT64_MAX;
        bank_done_for_visit_ = false;
        messages_.clear();
        spinners_.clear();
        monthly_.clear(); // MONTHLY
        minigames_.clear(); // MINIGAMES: the replay harness keeps the original closed mode
        session_->set_interactive_mini_games(interactive_minigames_ && !options_.auto_rounds);
        walk_sound_.stop(*audio_); // AUDIO
        auction_open_ = bankrupt_music_ = false;
        presentation_.clear();
        hold_ms_ = 0;
        presented_player_.reset();
        items_.reset();
        auction_room_.reset();
        intro_.skip();
        if (intro && !(options_.auto_rounds && !options_.play_effects)) { // replays keep their timing
            std::vector<std::uint8_t> characters;
            for (const auto &player : session_->world().players)
                if (player.state.controller_flags)
                    characters.push_back(player.state.character);
            intro_.start(characters);
            intro_ticks_ = 0;
            intro_pressed_ = false;
        }
        observe(); // the first turn's events queue behind the intro (presenting())
    }
    bool load(const fs::path &path) {
        try {
            const auto bytes = ui::read_file(path);
            auto seed = options_.seed;
            std::ifstream state(path.string() + ".rng");
            std::string signature;
            std::uint32_t stored_seed = 0;
            if (state >> signature >> stored_seed && signature == "RICH4-RNG-1")
                seed = stored_seed;
            install_session(World::from_save(SaveFile::decode(bytes)), seed);
            audio_->select_music(0); // AUDIO: loading moves on to the next playlist track (save_load.asm:978)
            music_director_.forget();
            ++loads_;
            load_failed_ = false;
            toast_ = "讀取完成";
            toast_until_ = SDL_GetTicks() + 2200;
            return true;
        } catch (const std::exception &error) {
            load_failed_ = true;
            toast_ = "無法讀取此存檔";
            toast_until_ = SDL_GetTicks() + 3500;
            std::cerr << "Load failed: " << error.what() << '\n';
            return false;
        }
    }
    void save() {
        if (!session_ || !session_->can_save())
            return;
        write_save(save_path());
        options_.load.clear(); // An imported DAT never shadows this newly saved native slot.
        ++saves_;
        toast_ = "存檔完成";
        toast_until_ = SDL_GetTicks() + 2200;
    }
    void write_save(const fs::path &path) {
        ui::write_file(path, session_->export_save().encode());
        const auto state = "RICH4-RNG-1 " + std::to_string(session_->random_state()) + '\n';
        ui::write_file(path.string() + ".rng", {state.begin(), state.end()});
    }
    // One tick of the core can emit a whole chain (e.g. a jailed player's notice, the next turn and
    // that player's roll). The original plays these strictly one after another, so presentation
    // (bubbles/boxes, dice, effect movies, sounds) goes through one ordered queue, and the core
    // clock only advances once the queue is empty. Bookkeeping for the replay harness stays immediate.
    struct PresentationStep {
        std::optional<GameEvent> event;
        std::optional<EffectEvent> effect;
    };
    bool presenting() const {
        return hold_ms_ > 0 || SDL_GetTicks() < stock_card_hold_until_ || intro_.active() || main_screen_.dice_busy() ||
               effects_.active() || messages_.active() ||
               spinners_.active() || monthly_.active() || auction_room_.sold_active() ||
               items_.magic_cast_active(); // MONTHLY / AUCTION / MAGIC full-screen sequences
    }
    void pump_presentation() {
        while (session_ && !presentation_.empty() && !presenting()) {
            auto step = std::move(presentation_.front());
            presentation_.pop_front();
            if (step.effect && step.effect->kind == EffectKind::pause) {
                // The original's movement-state countdown (EffectKind::pause): the board stays as it
                // is (e.g. the new owner flag / building) for N game ticks of 80 ms. Replays skip it.
                if (!(options_.auto_rounds && !options_.play_effects))
                    hold_ms_ = step.effect->object_kind * 80U;
            } else if (step.effect && spinners::handles(*step.effect))
                spinners_.enqueue(std::move(*step.effect)); // SPINNERS: computer spin / wheel slow-down
            else if (step.effect && native::monthly::handles(*step.effect))
                monthly_.enqueue(std::move(*step.effect)); // MONTHLY: dividend / lottery draw / month report
            else if (step.effect) {
                if (step.effect->kind == EffectKind::bankrupt)
                    bankrupt_music_ = true; // AUDIO: MIDI03 over the bankruptcy movie
                effects_.enqueue({std::move(*step.effect)}, session_.get()); // EFFECTS
            }
            else
                present(*step.event);
        }
    }
    void present(const GameEvent &event) {
        if (event.kind == GameEventKind::turn_started)
            presented_player_ = event.player;
        main_screen_.on_event(event, *session_, artwork_);
        if (auto item = messages::from_event(event, session_->world()))
            messages_.push(std::move(*item)); // Voices play when their bubble appears.
        if (event.message == "拍卖成交" || event.message == "地产拍卖流拍") // AUDIO: auction result
            auction_open_ = false;
        if (event.kind == GameEventKind::player_bankrupt)
            bankrupt_music_ = true; // AUDIO: MIDI03 until the bankruptcy presentation is over
        if (event.kind == GameEventKind::notice && event.message == "拍卖成交" && event.subject >= 0 &&
            !(options_.auto_rounds && !options_.play_effects)) // AUCTION: sold sequence (replays stay fast)
            auction_room_.start_sold(std::size_t(event.subject), event.amount, event.property);
        if (event.kind == GameEventKind::notice && event.message == "魔法屋施法" && // MAGIC: computer cast
            !(options_.auto_rounds && !options_.play_effects))
            items_.start_magic_cast(event.second_subject, event.value);
        if (!sound_effects_)
            return;
        if (event.sound) // AUDIO: core sound cue (god leaves, gift pickup, turn-around, 工程車)
            audio_->play_sound(*event.sound);
        if (event.message == "拍卖成交") { // AUDIO: gavel 63, "%d元成交" 135, "恭喜X購得此地！" 136+c
            audio_->play_sound(63);
            speak(135, false);
            if (event.subject >= 0 && std::size_t(event.subject) < session_->world().players.size())
                speak(native::voices::auction_buyer_voice(session_->world().players[std::size_t(event.subject)].state.character),
                      true);
        } else if (event.message == "地产拍卖流拍")
            speak(148, false); // 無人出價，宣佈流標。
        switch (event.kind) { // the dice sound fires at frame 30 of the dice FLC (MainScreen)
        case GameEventKind::land_purchased:
            audio_->play_sound(49);
            break;
        case GameEventKind::land_upgraded:
            audio_->play_sound(50);
            break;
        case GameEventKind::rent_paid:
            audio_->play_sound(55);
            break;
        default:
            break;
        }
    }
    void observe() {
        auto effects = session_->drain_effects();
        auto events = session_->drain_events();
        std::size_t next_effect = 0;
        const auto queue_effects_before = [&](std::size_t event_index) {
            while (next_effect < effects.size() && effects[next_effect].after_events <= event_index)
                presentation_.push_back({std::nullopt, std::move(effects[next_effect++])});
        };
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto &event = events[index];
            queue_effects_before(index);
            presentation_.push_back({event, std::nullopt});
            ++event_counts_[event.kind];
            if (event.kind == GameEventKind::dice_rolled && event.player < dice_by_player_.size())
                ++dice_by_player_[event.player];
            if (event.kind == GameEventKind::unsupported_event)
                unsupported_.push_back(event.message);
            items_.observe(event, session_->world());
            if (event.kind == GameEventKind::notice && event.message.starts_with("使用")) {
                for (unsigned i = 0; i < services::card_names.size(); ++i)
                    if (event.message == "使用" + std::string(services::card_names[i])) {
                        ++cards_used_;
                        card_ids_used_.insert(i + 1);
                    }
                for (unsigned i = 0; i < services::tool_names.size(); ++i)
                    if (event.message == "使用" + std::string(services::tool_names[i])) {
                        ++tools_used_;
                        tool_ids_used_.insert(i + 1);
                    }
            }
            if (event.kind == GameEventKind::date_changed || event.kind == GameEventKind::turn_started) {
                bank_done_for_visit_ = false;
                shop_done_for_visit_ = false;
                tested_cards_ = tested_tools_ = false;
                tested_dice_ = false;
            }
            if (event.kind != GameEventKind::moved && !event.message.empty()) {
                recent_.push_back(event.message);
                if (recent_.size() > 5)
                    recent_.erase(recent_.begin());
            }
            if (events_)
                events_ << "{\"event\":" << int(event.kind) << ",\"player\":" << event.player
                        << ",\"node\":" << event.node << ",\"amount\":" << event.amount
                        << ",\"day\":" << session_->completed_days()
                        << ",\"message\":" << quoted(event.message) << "}\n";
        }
        queue_effects_before(events.size());
        pump_presentation();
    }
    void draw() {
        buttons_.clear();
        messages_.retire();
        canvas_ = graphics::make_canvas(640, 480, ui::color(7, 30, 55));
        frame_.ticks = SDL_GetTicks();
        if (screen_ != last_screen_) {
            // Screens may be entered from anywhere (menu, toolbar, keys): (re)initialise on entry.
            previous_screen_ = last_screen_;
            last_screen_ = screen_;
            pad_.close();
            if (screen_ == Screen::settings)
                options_screen_.open({speed_, audio_->music_enabled(), sound_effects_, interactive_minigames_},
                                     session_ != nullptr);
            if (screen_ == Screen::wealth && session_)
                info_.open(session_->world().header.current_player);
            if (screen_ == Screen::ai_settings &&
                (!session_ || !ai_settings_.open(*session_, presented_player())))
                screen_ = last_screen_ = Screen::game; // nobody to hand over (all-computer game)
        }
        if (!bank_pending())
            bank_screen_.reset();
        auto host = make_host();
        if (session_) { // EFFECTS: pre-mutation board for effect commit frames (only while idle)
            std::array<std::uint8_t, 4> costumes{}; // BOARD MOVIES: gown/stripes poses
            for (std::size_t slot = 0; slot < costumes.size(); ++slot)
                costumes[slot] = session_->player_costume(slot);
            effects_.set_costumes(costumes);
            // Only while nothing emitted is still waiting to be shown: otherwise the world already
            // holds mutations whose pictures are pending (e.g. a thrown mine already on its node).
            if (presentation_.empty() && !presenting())
                effects_.remember(session_->world(), session_.get(), frame_.ticks);
        }
        if (session_)
            items_.run_deferred();
        items_.shot.clear();
        if (options_.auto_rounds)
            items_.clock = rendered_frames_ * 250;
        const bool menu_backdrop = screen_ == Screen::menu || ((screen_ == Screen::settings || screen_ == Screen::load) &&
                                                               previous_screen_ == Screen::menu) ||
                                   !session_;
        if (menu_backdrop && screen_ != Screen::new_game) {
            if (screen_ != Screen::menu)
                picture(artwork_.frame('d', 1, 0), 0, 0);
        } else if (session_ && screen_ != Screen::new_game)
            draw_board();
        if (session_ && screen_ == Screen::game && !items_.popup_active()) { // card art plays before its movie
            draw_effects(); // EFFECTS: overlay after the board, before toasts
            if (!effects_.active()) // SPINNERS: computer spins / wheel slow-down, in presentation order
                spinners_.draw_effect(frame_, session_->world(), spinner_elapsed(), spinners::skip_button);
        }
        if (session_ && screen_ == Screen::game && monthly_.active() && !effects_.active() && !spinners_.active())
            draw_monthly(); // MONTHLY: full screen, modal
        if (session_ && screen_ == Screen::game)
            draw_intro();
        if (session_ && screen_ == Screen::game && SDL_GetTicks() < stock_card_hold_until_) {
            buttons_.clear(); // the updated price page, as the original redraws it before returning
            auto host = make_host();
            stock_market_.draw_card_target(host, session_->world(), stock_card_player_, nullptr, 210, {});
        } else if (stock_card_hold_until_) {
            stock_card_hold_until_ = 0;
            stock_market_.selected.reset();
        }
        if (options_.minigame >= 0 && screen_ == Screen::menu) {
            draw_dev_minigame(); // MINIGAMES (dev): one game alone, replayed with the next seed
        } else if (screen_ == Screen::menu) {
            cleared_maps_ = 0; // END SCREENS: the main menu ends a campaign (loc_00401c8c)
            main_menu_.draw(host, {[this] {
                                       if (session_ && session_->phase() != GamePhase::finished)
                                           screen_ = Screen::game;
                                       else
                                           open_setup(0);
                                   },
                                   [this] { open_load(); },
                                   [this] { screen_ = Screen::settings; },
                                   [this] { running_ = false; },
                                   [this] { open_setup(1); },
                                   fs::exists(load_path()) || any_slot()});
        } else if (screen_ == Screen::new_game) {
            setup_.draw(host, [this](const screens::NewGameChoice &choice) { new_game(choice); },
                        [this] { screen_ = Screen::menu; });
        } else if (screen_ == Screen::settings) {
            buttons_.clear();
            options_screen_.draw(
                host, {[this](const screens::OptionsState &state) {
                           speed_ = state.speed;
                           if (audio_->music_enabled() != state.music)
                               audio_->set_music(state.music);
                           sound_effects_ = state.sound;
                           interactive_minigames_ = state.animation; // MINIGAMES: 動畫過程 (cfg+1)
                           if (session_)
                               session_->set_interactive_mini_games(state.animation && !options_.auto_rounds);
                       },
                       [this] { screen_ = back_screen(); },
                       [this](unsigned track) {
                           if (!audio_->music_enabled())
                               audio_->set_music(true);
                           audio_->select_music(track);
                           music_director_.forget(); // AUDIO: the pick drops the context stack
                       },
                       audio_->music_enabled() ? audio_->music_track() : 0,
                       [this] { open_setup(selected_stage_); },
                       [this] { end_to_title(); }, // 結束遊戲 in a game: back to the title (its EXIT quits)
                       [this] { help_.open(-1, -1); }}); // HELP: Options 遊戲說明, centred
        } else if (screen_ == Screen::load || screen_ == Screen::save) {
            buttons_.clear();
            slots_.draw(host, [this](int slot) { choose_slot(slot); }, [this] { screen_ = back_screen(); });
        } else if (screen_ == Screen::wealth) {
            buttons_.clear();
            info_.draw(host, session_->world(), [this] { screen_ = Screen::game; });
        } else if (screen_ == Screen::cards || screen_ == Screen::tools) {
            draw_inventory();
        } else if (screen_ == Screen::stocks) {
            draw_stocks();
        } else if (screen_ == Screen::trading) {
            draw_trading();
        } else if (screen_ == Screen::ai_settings && session_) {
            buttons_.clear();
            ai_settings_.draw(host, session_->world(), ai_settings_actions());
        }
        std::string overlay_key;
        if (screen_ == Screen::game && session_ && monthly_.active()) // MONTHLY: first frames of each screen
            overlay_key = std::string("monthly-") + monthly_.shot();
        if (screen_ == Screen::game && session_ && messages_.active()) {
            const auto kind = messages_.current()->kind;
            if (messages_.draw(frame_)) {
                static constexpr std::array<const char *, 5> kinds{"bubble", "box", "news", "fortune", "god"};
                overlay_key = std::string("overlay-") + kinds.at(static_cast<std::size_t>(kind));
            }
        }
        if (screen_ == Screen::game && session_ && items_.magic_cast_active() && !messages_.active()) {
            buttons_.clear(); // MAGIC: a computer's magic-house cast and Panel 20 movie
            items_.draw_magic_cast(frame_);
        }
        if (screen_ == Screen::game && session_ && auction_room_.sold_active() && !messages_.active()) {
            buttons_.clear(); // AUCTION: the sold sequence covers the board until it ends or is tapped
            auction_room_.draw_sold(host, session_->world());
        }
        if (items_.popup_active() && session_ &&
            (screen_ == Screen::game || screen_ == Screen::cards || screen_ == Screen::tools)) {
            buttons_.clear();
            items_.draw_popup(frame_);
        }
        if (pad_.active()) { // The number pad is modal over every screen.
            buttons_.clear();
            pad_.draw(host);
        }
        if (help_.active()) { // HELP: modal window over the current screen
            buttons_.clear();
            help_.draw(host);
        }
        if (!toast_.empty() && SDL_GetTicks() < toast_until_) {
            ui::fill(canvas_, {210, 439, 430, 474}, ui::paper);
            ui::border(canvas_, {210, 439, 430, 474}, ui::blue);
            font_.draw(canvas_, toast_, 320 - font_.measure(toast_) / 2, 447);
        }
        spinners_.finish_frame(); // SPINNERS: rattle loop only while a reel/wheel is visibly spinning
        const auto pixels = graphics::to_argb8888(canvas_);
        require(SDL_UpdateTexture(texture_.get(), nullptr, pixels.data(), 640 * 4));
        require(SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255));
        require(SDL_RenderClear(renderer_.get()));
        require(SDL_RenderTexture(renderer_.get(), texture_.get(), nullptr, nullptr));
        require(SDL_RenderPresent(renderer_.get()));
        ++rendered_frames_;
        if (options_.capture_every && !options_.output.empty() && rendered_frames_ % options_.capture_every == 0) {
            std::ostringstream name;
            name << "frame-" << std::setw(6) << std::setfill('0') << rendered_frames_ << ".ppm";
            graphics::write_ppm(canvas_, options_.output / name.str());
            std::ofstream(options_.output / "capture-times.txt", std::ios::app) // dev: real-time strips
                << name.str() << ' ' << SDL_GetTicks() << '\n';
        }
        if (options_.auto_rounds && !options_.output.empty() && !gallery_) {
            auto key = "screen-" + std::to_string(static_cast<int>(screen_));
            if (screen_ == Screen::game && session_ && session_->pending_decision())
                key = "decision-" + std::to_string(static_cast<int>(session_->pending_decision()->kind)) +
                      (selected_target_ ? "-selected" : "") +
                      (bank_pending() ? bank_screen_.at_atm() ? "-atm" : bank_screen_.at_desk() ? "-desk" : "-director"
                                      : "");
            if (pad_.active())
                key += "-pad";
            if (!overlay_key.empty()) // First few of each overlay kind, for visual review.
                key = overlay_key + "-" + std::to_string(std::min(++overlay_captures_[overlay_key], 4U));
            if (captured_frames_.insert(key).second)
                graphics::write_ppm(canvas_, options_.output / (key + ".ppm"));
            if (!items_.shot.empty() && ++item_shots_[items_.shot] <= 4) // settled frame of each item screen
                graphics::write_ppm(canvas_, options_.output / ("items-" + items_.shot + ".ppm"));
        }
    }

    // Full-screen new-game intro (screens/intro.hpp). Covers the board until it ends or is skipped.
    void draw_intro() {
        const auto now = SDL_GetTicks();
        const auto elapsed = intro_ticks_ ? static_cast<unsigned>(std::min<Uint64>(now - intro_ticks_, 100)) : 0;
        intro_ticks_ = now;
        if (!intro_.active())
            return;
        intro_.draw(canvas_, elapsed, [this](std::size_t sound) {
            if (sound_effects_)
                audio_->play_sound(sound);
        });
        if (!intro_.active())
            return;
        buttons_.clear(); // input is blocked while the intro plays
        if (options_.play_effects && !options_.output.empty() && now >= intro_capture_ && intro_frames_ < 400) {
            std::ostringstream name;
            name << "intro-" << std::setw(3) << std::setfill('0') << intro_frames_++ << ".ppm";
            graphics::write_ppm(canvas_, options_.output / name.str());
            intro_capture_ = now + 80;
        }
    }
    // EFFECTS: plays queued original animations (tools/screens/effects.cpp) over the board viewport.
    void draw_effects() {
        const auto now = SDL_GetTicks();
        const auto elapsed = effect_ticks_ ? static_cast<unsigned>(std::min<Uint64>(now - effect_ticks_, 100)) : 0;
        effect_ticks_ = now;
        if (!effects_.active()) {
            // The god's last movie frame stays under its arrival caption (fcn_0040e2a2 draws the
            // caption over it and waits 2400 ms; the next board redraw comes after).
            if (effects_.lingering()) {
                const auto caption = [](const GameEvent &event) {
                    return event.announcement == Announcement::god_arrival;
                };
                const bool shown = messages_.active() && messages_.current()->kind == messages::Item::Kind::god_caption;
                const bool next = !messages_.active() && !presentation_.empty() && presentation_.front().event &&
                                  caption(*presentation_.front().event);
                if (shown || next)
                    effects_.draw_linger(canvas_, *board_, session_->world(), static_cast<std::uint8_t>(rotation_), now);
                else
                    effects_.drop_linger();
            }
            return;
        }
        effects_.draw(canvas_, *board_, session_->world(), static_cast<std::uint8_t>(rotation_), elapsed, now,
                      [this](std::size_t sound) {
                          if (sound_effects_)
                              audio_->play_sound(sound);
                      });
        if (effects_.active())
            buttons_.clear(); // input is blocked while an effect plays, as in the original
        if (options_.play_effects && !options_.output.empty() && now >= effect_capture_ && effect_frames_ < 200) {
            graphics::write_ppm(canvas_, options_.output / ("effect-" + std::to_string(effect_frames_++) + ".ppm"));
            effect_capture_ = now + 300;
        }
    }
    // MONTHLY: dividend table / lottery draw / month-end report (screens/monthly.cpp) over everything.
    void draw_monthly() {
        const auto now = SDL_GetTicks();
        const auto elapsed = monthly_ticks_ ? static_cast<unsigned>(std::min<Uint64>(now - monthly_ticks_, 100)) : 0U;
        monthly_ticks_ = now;
        monthly_.draw(frame_, session_->world(), options_.auto_rounds ? 40U : elapsed * speed_);
    }
    // MINIGAMES (dev): --minigame plays one game standalone; headless autoplays it and writes frames.
    void draw_dev_minigame() {
        buttons_.clear();
        if (!minigames_.active()) {
            if (dev_minigame_rounds_++ && options_.headless) {
                running_ = false;
                return;
            }
            minigames_.set_fixed_step(options_.headless ? 40 : 0);
            minigames_.start(static_cast<minigames::core::Game>(options_.minigame),
                             options_.seed + dev_minigame_rounds_, 0, frame_, options_.headless,
                             options_.headless ? options_.output : fs::path{});
        }
        if (const auto result = minigames_.draw(frame_)) {
            std::cerr << "mini-game score " << result->score << ", rand() calls " << result->draws << '\n';
            minigames_.clear();
        }
    }
    unsigned spinner_elapsed() { // SPINNERS: real time between frames (replays: 40 ms per frame)
        const auto now = SDL_GetTicks();
        const auto elapsed = spinner_ticks_ ? static_cast<unsigned>(std::min<Uint64>(now - spinner_ticks_, 100)) : 0;
        spinner_ticks_ = now;
        return options_.auto_rounds ? 40U : elapsed;
    }
    void draw_board() {
        const auto &world = session_->world();
        sync_decision();
        // Follow the turn being presented, not the core's (possibly already advanced) current player.
        main_screen_.set_displayed_player(presentation_.empty() && !presenting() ? std::nullopt
                                                                                 : presented_player_);
        const auto &active = world.players[main_screen_.displayed_player(world)].state;
        std::vector<board::PlayerSprite> players;
        for (const auto &player : world.players) {
            const auto &state = player.state;
            if (!native::on_board(world, state.slot)) // BOARD MOVIES: away/confined players are not drawn
                continue;
            board::Point position{state.x, state.y};
            bool moving = false;
            auto heading = state.direction;
            if (const auto &animation = session_->animation(); animation && animation->player == state.slot) {
                const auto &movement = *animation;
                position.x = movement.from_x + (movement.to_x - movement.from_x) * int(movement.elapsed_ms) /
                                                   int(movement.duration_ms);
                position.y = movement.from_y + (movement.to_y - movement.from_y) * int(movement.elapsed_ms) /
                                                   int(movement.duration_ms);
                moving = true;
                // rich4.asm:6168-6179: every walk tick the heading (+16) is the direction of the step
                // being walked (node +14 -> +12), so the walker and anything it carries face it.
                if (movement.to_x != movement.from_x || movement.to_y != movement.from_y)
                    heading = rich4::direction::from_delta(movement.to_x - movement.from_x,
                                                           movement.to_y - movement.from_y);
            }
            players.push_back(
                {position, state.character, heading,
                 static_cast<std::uint8_t>((SDL_GetTicks() / 120) % 6),
                 static_cast<std::uint8_t>(state.traffic_method & 3U),
                 ui::player_color(state.color), state.slot, state.slot == active.slot, moving,
                 state.node_id < world.nodes.size() && (world.nodes[state.node_id].flags & 0x80000000U) != 0,
                 player.status.days_sleep_walking != 0, (state.controller_flags & 0x40U) != 0});
            players.back().special_pose = session_->player_costume(state.slot); // BOARD MOVIES: gown/stripes
            if (const auto toss = main_screen_.toss_frame(state.slot)) { // pre-roll toss pose
                players.back().tossing = true;
                players.back().animation_frame = *toss;
            }
        }
        { // EFFECTS: a player an ambulance/police/UFO/airplane movie still has to take away (or bring
          // back) stays where the original still draws them during the steps before that movie.
            std::vector<native::PendingActor> pending;
            for (const auto &step : presentation_)
                if (step.effect)
                    native::add_pending_actor(pending, *step.effect);
            std::array<std::uint8_t, 4> costumes{};
            for (std::size_t slot = 0; slot < costumes.size(); ++slot)
                costumes[slot] = session_->player_costume(slot);
            native::apply_pending_actors(players, world, pending,
                                         static_cast<std::uint8_t>((SDL_GetTicks() / 120) % 6), costumes);
            for (auto &player : players)
                player.active = player.slot == active.slot;
            effects_.set_pending_actors(std::move(pending));
        }
        std::vector<board::PropertyState> lands, facilities;
        for (std::size_t index = 1; index < world.lands.size(); ++index) {
            const auto &land = world.lands[index];
            lands.push_back({land.id, land.economy.owner_code, land.economy.level, land.economy.chain_store,
                             land.temporary_state}); // temporary: 漲價卡/查封卡 tint (map.mkf 26)
        }
        for (std::size_t index = 1; index < world.facilities.size(); ++index) {
            const auto &facility = world.facilities[index];
            facilities.push_back({facility.id, facility.owner_code, facility.level, facility.facility_type,
                                  facility.temporary_state});
        }
        // A purchase/upgrade not presented yet is not on the board yet: the original redraws the plot
        // with the new owner flag / building when it buys (core_actions:1051/439), after the 5-tick
        // walk-end pause, then holds 8 ticks (EffectKind::pause).
        for (auto step = presentation_.rbegin(); step != presentation_.rend(); ++step) {
            if (!step->event || step->event->node >= world.nodes.size())
                continue;
            const auto &event = *step->event;
            const bool bought = event.kind == GameEventKind::land_purchased;
            const bool raised = event.kind == GameEventKind::land_upgraded ||
                                (event.kind == GameEventKind::notice && // god post-purchase hook (fcn_0040f8be)
                                 (event.message == "福神帮助房屋升级" || event.message == "神明帮助房屋升级"));
            if (!bought && !raised)
                continue;
            const auto code = world.nodes[event.node].type_and_index;
            board::PropertyState *state = nullptr;
            if (code > 2000 && code < 4000 && code - 2000U <= lands.size())
                state = &lands[code - 2001U];
            else if (code > 4000 && code < 6000 && code - 4000U <= facilities.size())
                state = &facilities[code - 4001U];
            if (!state)
                continue;
            if (bought)
                state->owner = 0;
            else if (state->level)
                --state->level;
        }
        board::Point center{active.x, active.y};
        for (const auto &player : players)
            if (player.active)
                center = player.position;
        center = main_screen_.camera_center(center);
        if (target_center_ && session_->pending_decision())
            center = *target_center_;
        if (const auto speaker = messages_.focus(); speaker && *speaker < world.players.size()) {
            center = {world.players[*speaker].state.x, world.players[*speaker].state.y}; // Bubble speaker.
            for (const auto &player : players) // where the speaker is drawn (PendingActor)
                if (player.slot == *speaker)
                    center = player.position;
        }
        board::Camera camera{center, static_cast<std::uint8_t>(rotation_)};
        current_camera_ = camera;
        auto objects = native::object_sprites(world, players, static_cast<std::uint8_t>((SDL_GetTicks() / 120) % 6));
        native::hide_objects(objects, [this](board::Point node, std::uint8_t kind) { // EFFECTS: throws in flight
            if (effects_.throw_pending(node, kind))
                return true;
            return std::any_of(presentation_.begin(), presentation_.end(), [&](const PresentationStep &step) {
                return step.effect && step.effect->kind == EffectKind::object_throw &&
                       step.effect->object_kind == kind && step.effect->target.x == node.x &&
                       step.effect->target.y == node.y;
            });
        });
        std::vector<board::SpecialPlayerSprite> special_players;
        for (std::size_t index = 0; index < world.special_actors.size(); ++index) {
            const auto &actor = world.special_actors[index];
            if (actor.inactive_state || !actor.node_id || actor.node_id >= world.nodes.size())
                continue;
            board::Point position{actor.x, actor.y};
            bool moving = false;
            if (const auto &animation = session_->animation(); animation && animation->player == index + 4) {
                const auto &movement = *animation;
                position.x = movement.from_x + (movement.to_x - movement.from_x) * int(movement.elapsed_ms) /
                                                   int(movement.duration_ms);
                position.y = movement.from_y + (movement.to_y - movement.from_y) * int(movement.elapsed_ms) /
                                                   int(movement.duration_ms);
                moving = true;
            }
            special_players.push_back({position, static_cast<std::uint8_t>(index + 4), actor.direction,
                                       static_cast<std::uint8_t>((SDL_GetTicks() / 120) % 6), false, moving,
                                       (world.nodes[actor.node_id].flags & 0x80000000U) != 0,
                                       actor.days_sleep_walking != 0});
        }
        std::vector<board::PropertyState> companies;
        for (const auto &company : world.commercials)
            if (company.id)
                companies.push_back({company.id, company.owner_code, 0, 0});
        const auto owners = native::property_owners(world);
        board_->render(canvas_, {players, objects, lands, facilities, special_players, companies, owners}, camera);
        native::MainScreenActions actions;
        actions.help = [this] { help_.open(20, 60); }; // HELP: toolbar "?" opens at (20, 60)
        actions.options = [this] { screen_ = Screen::settings; };
        actions.ai = [this] { screen_ = Screen::ai_settings; };
        actions.load = [this] { open_load(); };
        actions.save = [this] { open_save(); };
        actions.query = [this] { screen_ = Screen::wealth; };
        actions.tools = [this] { screen_ = Screen::tools; };
        actions.cards = [this] { screen_ = Screen::cards; };
        actions.trading = [this] {
            if (session_->open_trading()) {
                screen_ = Screen::trading;
                trading_board_.reset();
            }
        };
        actions.stocks = [this] { screen_ = Screen::stocks; };
        actions.rotate = [this](unsigned increment) { rotate(increment); };
        actions.roll = [this] {
            if (presentation_.empty() && !presenting())
                session_->roll_dice();
        };
        actions.dice_count = [this](unsigned count) {
            const bool success = session_->set_dice_count(count);
            if (success)
                ++dice_selections_;
            notify_action(success);
        };
        actions.sound = [this](unsigned id) {
            if (sound_effects_)
                audio_->play_sound(id);
        };
        actions.can_save = session_->can_save();
        actions.can_load = fs::exists(load_path());
        // Map-tile popup (main-screen.md §7): only while GO waits and nothing else is shown; GO,
        // target picking and the minimap keep priority (their hits are registered first).
        const bool board_idle = screen_ == Screen::game && session_->phase() == GamePhase::human_ready &&
                                !session_->pending_decision() && !messages_.active() && presentation_.empty() &&
                                !presenting() && !main_screen_.dice_busy();
        if (!board_idle)
            tile_popup_.reset();
        else if (tile_popup_)
            native::tile_popup::draw(frame_, world, *tile_popup_);
        main_screen_.draw(frame_, *session_, rotation_, actions);
        if (board_idle)
            buttons_.push_back({native::tile_popup::hit_id, current_camera_.viewport, true,
                                [this] { toggle_tile_popup(frame_.pointer_x, frame_.pointer_y); }, false});
        // A decision appears only after everything emitted before it (e.g. a god's entrance movie and
        // caption before its spinner) has been shown, as the original blocks until then.
        if (screen_ == Screen::game && session_->pending_decision() && !messages_.active() &&
            presentation_.empty() && !presenting())
            draw_decision();
        draw_endgame(); // END SCREENS
    }
    // END SCREENS (screens/endgame.hpp): once a finished session's last presentation has played,
    // the original end sequence runs over the board. Exit codes as rich4_main.asm's 0x46caf8.
    void draw_endgame() {
        if (session_->phase() != GamePhase::finished || screen_ != Screen::game)
            return;
        if (endgame_session_ != session_.get()) {
            if (!presentation_.empty() || presenting())
                return;
            endgame_session_ = session_.get();
            start_endgame();
        }
        if (!endgame_.active())
            return;
        buttons_.clear();
        auto host = make_host();
        endgame_.draw(host, messages_.active(), endgame_actions());
    }
    void start_endgame() {
        const auto &world = session_->world();
        const auto stage = world.header.stage_id % 2U, map = world.header.map_id % 4U;
        if (const auto winner = session_->winning_player()) {
            // Winner's line (event slot 24), then fcn_004075c1 (exit code 2 with one human, else 3).
            const auto character = world.players.at(*winner).state.character;
            messages_.push(messages::speech_bubble(character, 24, std::nullopt));
            if (cleared_stage_ != stage)
                cleared_maps_ = 0;
            cleared_stage_ = stage;
            cleared_maps_ = static_cast<std::uint8_t>(cleared_maps_ | (1U << map));
            endgame_.open_stage_clear({stage, map, character, cleared_maps_, humans_at_start_ <= 1,
                                       options_.seed ^ static_cast<std::uint32_t>(session_->completed_days())});
        } else if (humans_at_start_ <= 1) {
            // The bankruptcy context music (MIDI03) is chosen by the music controller (update_audio).
            endgame_.open_play_again(); // the only human went bankrupt: fcn_00407842(0)
        } else {
            screen_ = Screen::menu; // several humans, all bankrupt: exit code 1
        }
    }
    screens::EndgameActions endgame_actions() {
        return {[this] {
                    // Game-over line (event slot 26) of the player whose turn ended the game.
                    const auto &world = session_->world();
                    const auto character =
                        world.players.at(world.header.current_player % world.players.size()).state.character;
                    messages_.push(messages::speech_bubble(character, 26, std::nullopt));
                },
                [this](bool load) {
                    screen_ = Screen::menu;
                    if (load)
                        open_load(); // exit code 4: the load screen; without saves the menu stays
                },
                [this](unsigned map) { // exit code 2: the campaign continues on the picked map
                    selected_stage_ = cleared_stage_;
                    selected_map_ = map;
                    open_setup(cleared_stage_);
                },
                [this] { screen_ = Screen::menu; }};
    }
    // Touch: a tap on the board shows the popup; the next tap anywhere dismisses it.
    void toggle_tile_popup(int x, int y) {
        if (tile_popup_) {
            tile_popup_.reset();
            return;
        }
        const auto code = native::tile_popup::instance_at(session_->world(), current_camera_, x, y);
        if (!code || native::tile_popup::lines(session_->world(), code).empty())
            return;
        tile_popup_ = native::tile_popup::Popup{x, y, code};
        if (sound_effects_)
            audio_->play_sound(1); // ref_00482322, as fcn_00417559
    }
    // 託管AI: the player whose turn is shown is preselected in the window.
    std::size_t presented_player() const {
        return presented_player_.value_or(session_ ? session_->world().header.current_player : 0);
    }
    screens::AiSettingsActions ai_settings_actions() {
        return {[this](std::size_t slot, const AiSettings &settings) {
                    const bool waits = session_->phase() != GamePhase::human_ready &&
                                       session_->phase() != GamePhase::finished;
                    // The core can only save on a waiting human turn, so while every human is on
                    // 託管 nothing (not even Android's background autosave) is written. Autosave now,
                    // before the hand-over, so a killed app resumes where control was handed over.
                    if (settings.autopilot && session_->can_save())
                        write_save(options_.save_directory / "Autosave.DAT");
                    if (session_->set_ai_settings(slot, settings) && waits)
                        notify_action(false, "設定將於本回合結束後生效");
                    observe(); // an idle hand-over starts the computer turn at once
                },
                [this] { screen_ = Screen::game; }};
    }
    native::items::Host item_host() {
        return {[this] { screen_ = Screen::game; },
                [this](bool success, std::string_view failure) { notify_action(success, failure); },
                [this](std::size_t index) { return respond(index); }};
    }
    void draw_inventory() {
        buttons_.clear();
        items_.draw_inventory(frame_, *session_, screen_ == Screen::cards, item_host());
    }
    void draw_stocks() {
        buttons_.clear();
        auto host = make_host();
        stock_market_.draw(host, session_->world(), session_->world().header.current_player, pad_, stock_actions());
    }
    void close_stocks() {
        end_amount_edit();
        pad_.close();
        stock_market_.reset();
        screen_ = Screen::game;
        notify_action(session_->close_stock_market(), "目前無法完成股市結算");
    }
    bool trade(bool buy, std::int32_t quantity) {
        end_amount_edit();
        const bool success = buy ? session_->buy_stock(selected_stock_, quantity)
                                 : session_->sell_stock(selected_stock_, quantity);
        if (success) {
            if (buy)
                ++stocks_bought_;
            else
                ++stocks_sold_;
        }
        notify_action(success, "休市、限價或可交易數量不足");
        return success;
    }
    void close_trading() {
        end_amount_edit();
        pad_.close();
        trading_board_.reset();
        screen_ = Screen::game;
        notify_action(session_->close_trading(), "目前無法完成買賣結算");
    }
    screens::TradingActions trading_actions() {
        return {[this](const trading::Listing &listing) {
                    const bool success = session_->post_offer(listing);
                    if (success)
                        ++offers_posted_;
                    notify_action(true);
                    return success;
                },
                [this](std::size_t index) {
                    const bool success = session_->cancel_offer(index);
                    if (success)
                        ++offers_cancelled_;
                    notify_action(true);
                    return success;
                },
                [this](std::size_t seller, std::size_t index) {
                    const bool success = session_->buy_offer(seller, index);
                    if (success)
                        ++offers_bought_;
                    notify_action(true);
                    return success;
                },
                [this] { close_trading(); }};
    }
    void draw_trading() {
        buttons_.clear();
        auto host = make_host();
        trading_board_.draw(host, session_->world(), session_->world().header.current_player, pad_,
                            trading_actions());
    }
    void sync_decision() {
        if (!session_->pending_decision()) {
            items_.end_visits();
            decision_key_.clear();
            selected_target_.reset();
            target_center_.reset();
            return;
        }
        const auto &decision = *session_->pending_decision();
        auto key = decision.prompt + ':' + std::to_string(static_cast<int>(decision.kind));
        for (const auto &option : decision.options)
            key += ':' + std::to_string(option.value);
        if (decision_key_ == key)
            return;
        end_amount_edit();
        pad_.close(); // a number pad never outlives the decision it was opened for
        decision_key_ = std::move(key);
        decision_page_ = 0;
        selected_target_.reset();
        target_center_.reset();
        if (decision.kind == DecisionKind::primary_stock)
            bank_amount_ = std::min(100, decision.max_amount);
    }
    bool map_decision() const {
        if (!session_->pending_decision())
            return false;
        const auto &options = session_->pending_decision()->options;
        return std::any_of(options.begin(), options.end(),
                           [](const auto &option) { return option.location.has_value(); });
    }
    void select_target(std::size_t index) {
        if (!session_ || !session_->pending_decision() ||
            index >= session_->pending_decision()->options.size())
            return;
        selected_target_ = index;
        const auto &option = session_->pending_decision()->options.at(index);
        if (option.location)
            target_center_ = board::Point{option.location->x, option.location->y};
    }
    void draw_map_decision(const GameDecision &pending) {
        // Original names for the targets (messages::localized); values and order are unchanged.
        const auto decision = messages::localized(pending, session_->world());
        ui::fill(canvas_, {440, 0, 640, 480}, ui::paper);
        font_.draw(canvas_, decision.prompt, 449, 16, 16, ui::blue, 181);
        constexpr std::size_t page_size = 8;
        const auto first = decision_page_ * page_size;
        items_.draw_targets(frame_, decision, current_camera_, selected_target_, [this](std::size_t index) {
            if (selected_target_ != index) {
                select_target(index); // first tap selects and centres the camera
                return;
            }
            if (sound_effects_)
                audio_->play_sound(2);
            respond(index); // second tap on the same target confirms
        });
        font_.draw(canvas_, "點地圖上的目標，再點一次確認", 449, 66, 12, ui::color(144, 38, 17));
        for (std::size_t index = 0; index < decision.options.size(); ++index) {
            const auto &option = decision.options[index];
            if (index >= first && index < first + page_size) {
                const int y = 91 + int(index - first) * 34;
                button(210 + int(index), {448, y, 633, y + 30},
                       (option.location ? std::to_string(index + 1) + " " : "") + option.label,
                       [this, index] {
                           if (!session_->pending_decision() ||
                               index >= session_->pending_decision()->options.size())
                               return;
                           if (session_->pending_decision()->options[index].location)
                               select_target(index);
                           else
                               respond(index);
                       });
                if (selected_target_ == index)
                    ui::border(canvas_, {450, y + 2, 631, y + 28}, ui::blue);
            }
        }
        button(203, {448, 369, 538, 398}, "上一頁", [this] { --decision_page_; }, decision_page_ > 0);
        button(
            204, {543, 369, 633, 398}, "下一頁", [this] { ++decision_page_; },
            first + page_size < decision.options.size());
        button(
            207, {449, 420, 560, 462}, "確認目標",
            [this] {
                if (selected_target_)
                    respond(*selected_target_);
            },
            selected_target_.has_value());
        const auto cancel = std::find_if(decision.options.begin(), decision.options.end(),
                                         [](const auto &option) { return option.label == "取消"; });
        if (cancel != decision.options.end()) {
            const auto index = static_cast<std::size_t>(cancel - decision.options.begin());
            button(208, {567, 420, 632, 462}, "取消", [this, index] { respond(index); });
        }
    }
    void draw_decision() {
        buttons_.clear(); // A modal must never activate covered board controls.
        const auto &decision = *session_->pending_decision();
        if (voiced_decision_key_ != decision_key_) { // AUDIO: voices when a decision first shows
            voiced_decision_key_ = decision_key_;
            voice_decision_shown(decision);
        }
        if (decision.kind == DecisionKind::bank) {
            auto host = make_host();
            bank_screen_.draw(host, session_->world(), decision, pad_, bank_actions());
            return;
        }
        if (decision.kind == DecisionKind::primary_stock) {
            auto host = make_host();
            screens::draw_primary_stock(host, session_->world(), decision, pad_, bank_actions());
            return;
        }
        if (decision.kind == DecisionKind::lottery || decision.kind == DecisionKind::auction) {
            auto host = make_host();
            const auto answer = [this](std::size_t index) { respond(index); };
            if (decision.kind == DecisionKind::lottery)
                screens::draw_lottery(host, session_->world(), decision, answer, &lottery_farewell_,
                                      [this](bool picked) -> std::uint32_t { // ui_letou.asm:1227
                                          if (!sound_effects_)
                                              return 0;
                                          if (picked)
                                              audio_->play_sound(31);
                                          return audio_->play_speech(picked ? 14 : 16);
                                      });
            else
                auction_room_.draw(host, session_->world(), decision, answer);
            return;
        }
        if (map_decision()) {
            draw_map_decision(decision);
            return;
        }
        if (decision.kind == DecisionKind::action_target && // 紅卡 / 黑卡: pick a company on the market page
            std::any_of(decision.options.begin(), decision.options.end(), [](const auto &option) {
                return option.subject && option.subject->kind == CardTargetKind::stock;
            })) {
            buttons_.clear();
            auto host = make_host();
            const auto player = decision.player;
            stock_market_.draw_card_target(host, session_->world(), player, &decision, 210,
                                           [this, player](std::size_t index) {
                                               const bool used = session_->pending_decision() &&
                                                                 session_->pending_decision()->options.at(index).value >= 0;
                                               respond(index);
                                               if (used && !options_.auto_rounds) { // ST:2205: redraw, wait 1000 ms
                                                   stock_card_player_ = player;
                                                   stock_card_hold_until_ = SDL_GetTicks() + 1000;
                                               } else
                                                   stock_market_.selected.reset();
                                           });
            return;
        }
        if (items_.draw_decision(frame_, *session_, item_host()))
            return;
        if (minigames::handles(decision)) { // MINIGAMES: full-screen game; its window closing submits
            if (const auto result = minigames_.draw_decision(frame_, decision))
                session_->submit_mini_game(result->score, result->draws); // `decision` is gone now
            return;
        }
        if (spinners::handles(decision)) { // SPINNERS: slot machine / cupid wheel; STOP keeps id 210
            spinners_.draw_decision(frame_, session_->world(), decision, 210, [this] { respond(0); });
            return;
        }
        if (facility_ui::handles(decision)) { // FACILITY: fcn_00440aac strip; ids stay 210+option index
            auto host = make_host();
            facility_picker_.draw(host, decision, decision_key_, [this](std::size_t index) { respond(index); });
            return;
        }
        if (messages::is_yes_no(decision)) {
            // Original §2.2 question box + Data.mkf 440 strip; ids stay 210+option index.
            // Card prompts (free/frame card) list YES first; their values are not 1/0.
            const bool card = decision.kind == DecisionKind::card_target;
            const auto yes = card ? 0 : yes_no_index(1), no = card ? 1 : yes_no_index(0);
            messages::draw_yes_no(frame_, messages::yes_no_question(decision, session_->world()), 210 + int(yes),
                                  [this, yes] { respond(yes); }, 210 + int(no), [this, no] { respond(no); });
            return;
        }
        return draw_choice_dialog(messages::localized(decision, session_->world()));
    }
    void draw_choice_dialog(const GameDecision &decision) {
        constexpr std::size_t page_size = 8;
        const bool multiple = decision.options.size() > 3;
        const int top = multiple ? 52 : 115, bottom = multiple ? 438 : 360;
        dialog({72, top, 570, bottom}, "請選擇");
        font_.draw(canvas_, decision.prompt, 97, top + 50, 18, ui::ink, 448);
        if (decision.kind == DecisionKind::shop || decision.kind == DecisionKind::rescue)
            font_.draw(canvas_,
                       "可用點數 " +
                           std::to_string(session_->world().players.at(decision.player).state.points),
                       110, top + 110, 18, ui::blue);
        else if (decision.amount)
            font_.draw(canvas_,
                       decision.kind == DecisionKind::roulette ? "轉盤 " + std::to_string(decision.amount)
                                                               : ui::money(decision.amount),
                       110, top + 110, 22, ui::blue);
        const int width =
            std::min(160, 430 / static_cast<int>(std::max<std::size_t>(1, decision.options.size())));
        const auto first = decision_page_ * page_size;
        for (std::size_t index = first; index < std::min(first + page_size, decision.options.size());
             ++index) {
            const int position = int(index - first);
            const int x = multiple ? 93 + position % 2 * 232 : 105 + position * (width + 8);
            const bool bank = decision.kind == DecisionKind::bank;
            const int y = multiple ? (bank ? 200 + position / 2 * 36 : 208 + position / 2 * 44) : 290;
            button(210 + int(index), {x, y, x + (multiple ? 220 : width), y + (bank ? 32 : 36)},
                   decision.options[index].label, [this, index] { respond(index); });
        }
        if (decision.options.size() > page_size) {
            button(203, {105, 392, 290, 424}, "上一頁", [this] { --decision_page_; }, decision_page_ > 0);
            button(
                204, {335, 392, 520, 424}, "下一頁", [this] { ++decision_page_; },
                first + page_size < decision.options.size());
        }
    }
    std::size_t yes_no_index(std::int32_t value) const {
        const auto &options = session_->pending_decision()->options;
        for (std::size_t i = 0; i < options.size(); ++i)
            if (options[i].value == value)
                return i;
        return 0;
    }
    bool respond(std::size_t index) {
        const auto &decision = session_->pending_decision();
        if (!decision || minigames::handles(*decision)) // MINIGAMES: only the game's end submits
            return false;
        end_amount_edit();
        const auto amount =
            decision->kind == DecisionKind::bank || decision->kind == DecisionKind::primary_stock
                ? bank_amount_
                : 0;
        const auto kind = decision->kind; // AUDIO: answer sounds below
        const auto value = index < decision->options.size() ? decision->options[index].value : 0;
        if (!session_->respond(index, amount)) {
            toast_ = "金額、點數或持有數量不足";
            toast_until_ = SDL_GetTicks() + 2200;
            return false;
        }
        if (sound_effects_ && kind == DecisionKind::auction && value >= 1 && value <= 5)
            audio_->play_sound(29); // ui_auction.asm:1776 bid
        return true;
    }
    // split: release on a later frame, so press-and-hold feedback is exercised and visible.
    void push_button(int id, bool split = false) {
        for (const auto &button : buttons_)
            if (button.id == id && button.enabled) {
                SDL_Event event{};
                event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                event.button.windowID = SDL_GetWindowID(window_.get());
                event.button.which = 0;
                event.button.button = SDL_BUTTON_LEFT;
                require(SDL_RenderCoordinatesToWindow(
                    renderer_.get(), float(button.rectangle.left + button.rectangle.right) / 2,
                    float(button.rectangle.top + button.rectangle.bottom) / 2, &event.button.x,
                    &event.button.y));
                require(SDL_PushEvent(&event));
                event.type = SDL_EVENT_MOUSE_BUTTON_UP;
                if (split)
                    test_release_ = event;
                else
                    require(SDL_PushEvent(&event));
                return;
            }
        throw std::runtime_error("Native UI replay cannot find enabled button " + std::to_string(id));
    }
    void drive_test() {
        if (test_stage_ == 0) {
            push_button(options_.start_new || options_.load.empty()
                            ? (selected_stage_ ? screens::ids::menu_new_stage : screens::ids::menu_start)
                            : screens::ids::menu_load);
            test_stage_ = 1;
            return;
        }
        if (screen_ == Screen::new_game && test_stage_ != 5) { // stage 5: leaving the end screens
            if (setup_.map() != selected_map_)
                push_button(screens::ids::setup_map + int(selected_map_));
            else if (setup_.picks().empty())
                push_button(screens::ids::setup_character + int(selected_character_));
            else
                push_button(screens::ids::setup_ok);
            return;
        }
        if (screen_ == Screen::save) {
            push_button(screens::ids::save_slot + 1);
            return;
        }
        if (screen_ == Screen::load) {
            push_button(screens::ids::load_slot + 1);
            return;
        }
        if (!session_) {
            if (load_failed_)
                throw std::runtime_error("Native UI replay could not load its initial save");
            return;
        }
        if (screen_ == Screen::game && monthly_.active())
            return; // MONTHLY: a full-screen monthly screen covers the controls (closes by itself)
        if (screen_ == Screen::game && messages_.active())
            return; // An overlay is showing for exactly this frame (instant mode); controls are covered.
        if (auction_room_.sold_active() || items_.magic_cast_active())
            return; // --play-effects: the sold sequence / computer magic cast covers the controls
        if (session_->pending_decision() && (!presentation_.empty() || presenting()))
            return; // The decision is not shown until its preceding presentation has played.
        if (options_.play_effects && session_->pending_decision() && // dev strips: let the reels spin
            spinners::handles(*session_->pending_decision()) && ++test_spinner_frames_ < 30)
            return;
        test_spinner_frames_ = 0;
        if (test_cancel_pending_) {
            if (session_->phase() != GamePhase::human_ready || session_->pending_decision() ||
                session_->world().export_save() != test_cancel_save_ ||
                session_->random_state() != test_cancel_random_ || input_events_ != test_cancel_inputs_ + 1)
                throw std::runtime_error("Batched cancel did not preserve state or executed a stale control");
            test_cancel_pending_ = false;
            test_batched_cancel_ = true;
        }
        if (const auto &decision = session_->pending_decision()) {
            const auto &player = session_->world().active_player().state;
            const auto progress = decision_key_ + ':' + std::to_string(decision->amount) + ':' +
                                  std::to_string(session_->random_state()) + ':' +
                                  std::to_string(player.cash) + ':' + std::to_string(player.bank_balance) +
                                  ':' + std::to_string(player.points);
            if (progress == test_progress_)
                ++test_unchanged_frames_;
            else {
                test_progress_ = progress;
                test_unchanged_frames_ = 0;
            }
            if (test_unchanged_frames_ > 100) {
                graphics::write_ppm(canvas_, options_.output / "stalled.ppm");
                throw std::runtime_error("Native UI replay stalled at decision: " + decision->prompt);
            }
        } else {
            test_progress_.clear();
            test_unchanged_frames_ = 0;
        }
        if (pad_.active()) {
            // Type the amount on the number pad, one key per frame, then Enter.
            std::int32_t target = 1;
            if (const auto &decision = session_->pending_decision())
                target = decision->kind == DecisionKind::primary_stock ? std::min(100, decision->max_amount) : 1000;
            else if (screen_ == Screen::trading)
                target = trading_board_.suggestion();
            push_button(pad_.next_key_towards(target));
            return;
        }
        if (test_release_) {
            require(SDL_PushEvent(&*test_release_));
            test_release_.reset();
            return;
        }
        if (items_.has_deferred())
            return; // a released tile or timeout acts at the start of the next frame
        if (items_.popup_active()) {
            push_button(native::items::popup_skip_id);
            return;
        }
        if (screen_ == Screen::cards || screen_ == Screen::tools) {
            // Release on the tile uses it; if it could not be used the grid stays open, so go back.
            if (test_inventory_pushed_)
                push_button(native::items::back_id);
            else
                push_button(300 + int(test_inventory_id_), true);
            test_inventory_pushed_ = !test_inventory_pushed_;
            return;
        }
        test_inventory_pushed_ = false;
        if (screen_ == Screen::stocks) {
            if (screens::market_closed(session_->world()))
                push_button(screens::ids::stock_exit);
            else if (test_stock_stage_ == 1) {
                push_button(430);
                ++test_stock_stage_;
            } else if (test_stock_stage_ == 2) {
                push_button(431);
                ++test_stock_stage_;
            } else
                push_button(60);
            return;
        }
        if (screen_ == Screen::trading) {
            using View = screens::TradingBoard::View;
            const auto view = trading_board_.view();
            if (view == View::detail) {
                // Act once on the opened offer (撤件 / 購買), then close the card if it is still open.
                push_button(test_trade_acted_ ? screens::ids::trade_detail_exit : screens::ids::trade_act);
                test_trade_acted_ = !test_trade_acted_;
                return;
            }
            test_trade_acted_ = false;
            if (view != View::board) {
                // Post the first listable item: SALE menu category, then the item in its picker.
                const auto items = trading::listable_items(session_->world(), session_->world().header.current_player);
                if (items.empty())
                    throw std::runtime_error("Trading fixture has nothing to list");
                push_button(view == View::menu ? screens::ids::trade_category + int(items.front().kind) - 1
                                               : screens::ids::trade_item);
                return;
            }
            if (!offers_posted_) {
                push_button(480);
                return;
            }
            const auto quotes = trading::quotes(session_->world());
            const auto actor = session_->world().header.current_player;
            for (std::size_t i = 0; i < quotes.size(); ++i) {
                const bool own = quotes[i].seller == actor;
                if ((!offers_cancelled_ && own) ||
                    (offers_cancelled_ && !offers_bought_ && !own &&
                     !test_trade_attempts_.contains({quotes[i].seller, quotes[i].index}) &&
                     quotes[i].offer.price <= session_->world().active_player().state.cash)) {
                    if (std::any_of(buttons_.begin(), buttons_.end(), [i](const auto &button) {
                            return button.id == 600 + int(i) && button.enabled;
                        })) {
                        test_trade_attempts_.insert({quotes[i].seller, quotes[i].index});
                        push_button(600 + int(i));
                    }
                    return;
                }
            }
            if (!offers_cancelled_ || !offers_bought_)
                throw std::runtime_error("Trading fixture has no usable offer for its requested interaction");
            push_button(481);
            return;
        }
        if (test_stage_ == 1 && session_->can_save()) {
            if (options_.overlay_gallery)
                dump_overlay_gallery();
            ui::write_file(options_.output / "before.DAT", session_->export_save().encode());
            graphics::write_ppm(canvas_, options_.output / "before.ppm");
            test_stage_ = 2;
        }
        if (test_stage_ == 2 &&
            (session_->completed_days() >= options_.auto_rounds ||
             session_->phase() == GamePhase::finished) &&
            session_->can_save()) {
            test_completed_days_ = session_->completed_days();
            test_finished_ = session_->phase() == GamePhase::finished;
            expected_reload_ = session_->export_save().encode();
            expected_random_ = session_->random_state();
            ui::write_file(options_.output / "after.DAT", expected_reload_);
            graphics::write_ppm(canvas_, options_.output / "after.ppm");
            if (test_finished_)
                save(); // END SCREENS: the original end screens offer no save control
            else
                push_button(71);
            test_stage_ = 3;
            return;
        }
        if (test_stage_ == 3) {
            test_load_count_before_reload_ = loads_;
            if (test_finished_) {
                test_stage_ = 5; // leave the end screen below, then reload through the main menu
            } else {
                push_button(72);
                test_stage_ = 4;
            }
            return;
        }
        if (test_stage_ == 5) { // END SCREENS: the stage-clear map pick opens the setup; EXIT returns to the
                                // menu. PLAY AGAIN opens Load, whose slot the load branch above picks.
            if (loads_ != test_load_count_before_reload_)
                test_stage_ = 4;
            else if (screen_ == Screen::menu) {
                push_button(screens::ids::menu_load);
                test_stage_ = 4;
            } else if (screen_ == Screen::new_game) {
                if (!setup_.busy())
                    push_button(screens::ids::setup_exit);
            } else if (endgame_.exit_button() >= 0)
                push_button(endgame_.exit_button());
            return;
        }
        if (test_stage_ == 4) {
            if (load_failed_ || loads_ != test_load_count_before_reload_ + 1)
                throw std::runtime_error("Final reload failed to install a newly loaded game");
            if (!session_->can_save())
                throw std::runtime_error("Reload did not restore a playable human turn");
            reload_identical_ = session_->export_save().encode() == expected_reload_;
            reload_random_identical_ = session_->random_state() == expected_random_;
            finish_test();
            return;
        }
        if (test_invalid_load_stage_ == 1 || test_invalid_load_stage_ == 3) {
            if (!load_failed_ || session_->export_save().encode() != test_recovery_save_ ||
                session_->random_state() != test_recovery_random_)
                throw std::runtime_error("Invalid-save recovery changed or lost the active game");
            options_.load = test_recovery_path_;
            ++test_invalid_load_stage_;
        }
        if (options_.exercise_recovery && test_invalid_load_stage_ < 4 &&
            session_->phase() == GamePhase::human_ready) {
            test_recovery_save_ = session_->export_save().encode();
            test_recovery_random_ = session_->random_state();
            test_recovery_path_ = options_.load;
            options_.load = options_.output / "deliberately-invalid.DAT";
            if (test_invalid_load_stage_ == 0)
                ui::write_file(options_.load, {1, 2, 3});
            else {
                auto invalid = test_recovery_save_;
                invalid.at(0x23) = 255; // Structurally valid DAT, invalid player character.
                ui::write_file(options_.load, invalid);
            }
            ++test_invalid_load_stage_;
            push_button(72);
            return;
        }
        if (options_.exercise_recovery && !test_batched_cancel_ && map_decision()) {
            const auto marker = std::find_if(buttons_.begin(), buttons_.end(), [](const auto &button) {
                return button.id >= 5000 && button.enabled;
            });
            const auto cancel = std::find_if(buttons_.begin(), buttons_.end(), [](const auto &button) {
                return button.id == 208 && button.enabled;
            });
            if (marker != buttons_.end() && cancel != buttons_.end()) {
                const auto marker_id = marker->id;
                test_cancel_save_ = session_->world().export_save();
                test_cancel_random_ = session_->random_state();
                test_cancel_inputs_ = input_events_;
                push_button(208);
                push_button(marker_id);
                test_cancel_pending_ = true;
                tested_cards_ = tested_tools_ = false;
                return;
            }
        }
        if (session_->phase() == GamePhase::human_ready && !main_screen_.human_turn_shown(*session_))
            return; // like a player, wait until earlier turns have been shown and GO is up
        if (session_->phase() == GamePhase::human_ready) {
            const auto &player = session_->world().active_player().state;
            if (options_.exercise_trading && (!offers_posted_ || !offers_cancelled_ || !offers_bought_)) {
                push_button(80);
                return;
            }
            if (test_stock_stage_ == 0) {
                stock_market_.selected = 7;
                test_stock_stage_ = 1;
                push_button(79);
                return;
            }
            if (!tested_cards_) {
                tested_cards_ = true;
                std::vector<unsigned> available;
                for (const auto id : player.cards)
                    if (active_card(id))
                        available.push_back(id);
                if (!available.empty()) {
                    test_inventory_id_ =
                        available[(options_.seed + session_->completed_days()) % available.size()];
                    if (options_.exercise_card && !card_ids_used_.contains(options_.exercise_card) &&
                        std::find(available.begin(), available.end(), options_.exercise_card) !=
                            available.end())
                        test_inventory_id_ = options_.exercise_card;
                    push_button(77);
                    return;
                }
            }
            if (!tested_tools_) {
                tested_tools_ = true;
                std::vector<unsigned> available;
                for (unsigned i = 0; i < services::tool_names.size(); ++i)
                    if (player.tool_amounts[i])
                        available.push_back(i + 1);
                if (!available.empty()) {
                    test_inventory_id_ =
                        available[(options_.seed + session_->completed_days()) % available.size()];
                    if (options_.exercise_tool && !tool_ids_used_.contains(options_.exercise_tool) &&
                        std::find(available.begin(), available.end(), options_.exercise_tool) !=
                            available.end())
                        test_inventory_id_ = options_.exercise_tool;
                    push_button(78);
                    return;
                }
            }
            if (!tested_dice_) {
                tested_dice_ = true;
                if (std::any_of(buttons_.begin(), buttons_.end(),
                                [](const auto &button) { return button.id == 101 && button.enabled; })) {
                    push_button(101);
                    return;
                }
            }
            push_button(100);
        } else if (const auto &decision = session_->pending_decision()) {
            std::size_t choice = 0;
            if (decision->kind == DecisionKind::bank) {
                // ATM: deposit 1000 once per visit by typing on the ATM keys, then the loan desk's EXIT.
                const auto &player = session_->world().active_player().state;
                if (bank_screen_.at_atm()) {
                    if (bank_done_for_visit_ || player.cash < 1000) {
                        bank_done_for_visit_ = true;
                        push_button(screens::ids::atm_exit);
                    } else if (bank_screen_.atm_mode() != 1)
                        push_button(screens::ids::atm_deposit);
                    else if (const auto &digits = bank_screen_.atm_digits(); digits == "1000") {
                        bank_done_for_visit_ = true;
                        push_button(screens::ids::atm_enter);
                    } else
                        push_button(digits == "0"                      ? screens::ids::atm_digit(1)
                                    : std::string("1000").starts_with(digits) ? screens::ids::atm_digit(0)
                                                                              : screens::ids::atm_clear);
                } else
                    push_button(bank_screen_.at_director() ? screens::ids::director_exit : screens::ids::desk_exit);
                return;
            } else if (decision->kind == DecisionKind::shop) {
                const auto &player = session_->world().players.at(decision->player).state;
                for (std::size_t i = 0; i < decision->options.size(); ++i)
                    if (!decision->options[i].value)
                        choice = i;
                if (!shop_done_for_visit_) {
                    for (std::size_t i = 0; i < decision->options.size(); ++i) {
                        const auto action = decision->options[i].value;
                        if ((action > 100 && action <= 130 &&
                             player.points >= services::card_prices[action - 101] &&
                             card_count(player.cards) < 15) ||
                            (action > 200 && action <= 208 &&
                             player.points >= services::tool_prices[action - 201] &&
                             player.tool_amounts[action - 201] < 9)) {
                            choice = i;
                            break;
                        }
                    }
                    shop_done_for_visit_ = true;
                }
                if (const auto pending = items_.shop_pending_value()) // confirm the item tapped once
                    for (std::size_t i = 0; i < decision->options.size(); ++i)
                        if (decision->options[i].value == *pending) {
                            choice = i; // duplicate shelf rows all answer with the first option
                            break;
                        }
            } else if (decision->kind == DecisionKind::magic_effect) {
                const auto desired = static_cast<int>((options_.seed + session_->completed_days()) % 12);
                for (std::size_t i = 0; i < decision->options.size(); ++i)
                    if (decision->options[i].value == desired)
                        choice = i;
            } else if (decision->kind == DecisionKind::rescue) {
                const auto points = session_->world().players.at(decision->player).state.points;
                for (std::size_t i = 0; i < decision->options.size(); ++i) {
                    const auto target = decision->options[i].value;
                    if (target < 0 || points >= (target < 4 ? 30 : 300)) {
                        choice = i;
                        break;
                    }
                }
            }
            if (decision_page_ != choice / 8) {
                decision_page_ = choice / 8;
                return;
            }
            if (decision->kind == DecisionKind::shop && choice < decision->options.size() &&
                items_.shop_show_option(decision->options[choice].value))
                return; // the card or tool shelf must be on screen first
            if (map_decision() && selected_target_ == choice)
                push_button(207);
            else {
                const auto marker = 5000 + int(choice);
                const bool visible_marker =
                    map_decision() &&
                    std::any_of(buttons_.begin(), buttons_.end(), [marker](const auto &button) {
                        return button.id == marker && button.enabled;
                    });
                push_button(visible_marker ? marker : 210 + int(choice));
            }
        } else if (session_->phase() == GamePhase::finished)
            throw std::runtime_error("Match ended before the requested replay duration");
    }
    // Dev tool (--gallery DIR): renders each original-style menu/finance screen, including
    // decisions that only occur mid-game (bank, primary shares), for visual review.
    bool press(int id, const std::function<void()> &redraw = {}) {
        if (redraw)
            redraw();
        else
            draw();
        for (const auto &button : buttons_)
            if (button.id == id && button.enabled) {
                auto action = button.action;
                buttons_.clear();
                action();
                return true;
            }
        return false;
    }
    int gallery() {
        fs::create_directories(options_.gallery);
        options_.save_directory = options_.gallery / "saves";
        fs::create_directories(options_.save_directory);
        const auto shot = [this](const std::string &name) {
            draw();
            graphics::write_ppm(canvas_, options_.gallery / (name + ".ppm"));
        };
        const auto point = [this](int x, int y, bool down = false) {
            frame_.pointer_x = x;
            frame_.pointer_y = y;
            frame_.pointer_down = down;
        };
        point(190, 380);
        shot("menu");
        point(-1, -1);
        press(screens::ids::menu_start);
        shot("new-game");
        press(screens::ids::setup_character + 10);
        press(screens::ids::setup_character + 3);
        point(200, 50);
        shot("new-game-picked");
        point(-1, -1);
        press(screens::ids::setup_dropdown + 1);
        point(580, 336);
        shot("new-game-dropdown");
        point(-1, -1);
        press(screens::ids::setup_dropdown_close);
        press(screens::ids::setup_ok);
        if (!session_)
            throw std::runtime_error("Gallery could not start a game");
        shot("board");
        write_save(slot_path(0));
        write_save(slot_path(2));
        screen_ = Screen::settings;
        shot("options-in-game");
        open_save();
        point(300, 200);
        shot("save");
        point(-1, -1);
        screen_ = Screen::game;
        draw();
        open_load();
        point(300, 100);
        shot("load");
        point(-1, -1);
        screen_ = Screen::wealth;
        shot("info-assets");
        press(screens::ids::info_page + 1);
        shot("info-properties");
        press(screens::ids::info_page + 2);
        shot("info-stocks");
        screen_ = Screen::stocks;
        stock_market_.selected = 3;
        shot("stocks-prices");
        press(screens::ids::stock_page);
        shot("stocks-holdings");
        press(screens::ids::stock_page);
        stock_market_.info_open = true;
        shot("stocks-info");
        stock_market_.info_open = false;
        draw();
        if (press(screens::ids::stock_buy)) {
            press(screens::ids::pad_digit(5));
            shot("stocks-pad");
        }
        pad_.close();
        stock_market_.reset();
        { // 紅卡 target mode: trading off, row 5 outlined by the first tap.
            GameDecision red{DecisionKind::action_target, session_->world().header.current_player, 0, 0, "紅卡：選擇目標", {}};
            for (std::uint16_t i = 0; i < session_->world().stocks.size(); ++i)
                red.options.push_back({"", std::int32_t((unsigned(CardTargetKind::stock) << 16U) | i), std::nullopt,
                                       CardTarget{CardTargetKind::stock, i}});
            red.options.push_back({"取消", -1});
            stock_market_.selected = 5;
            buttons_.clear();
            canvas_ = graphics::make_canvas(640, 480, 0);
            auto host = make_host();
            stock_market_.draw_card_target(host, session_->world(), red.player, &red, 210, [](std::size_t) {});
            graphics::write_ppm(canvas_, options_.gallery / "stocks-red-card.ppm");
            stock_market_.reset();
        }
        screen_ = Screen::game;
        // Trading board: post a tool through SALE -> picker -> price pad, then open it.
        if (session_->open_trading()) {
            screen_ = Screen::trading;
            shot("trading-board-empty");
            press(screens::ids::trade_sale);
            point(500, 140);
            shot("trading-menu");
            point(-1, -1);
            press(screens::ids::trade_category + 2);
            shot("trading-tools");
            const auto items = trading::listable_items(session_->world(), session_->world().header.current_player);
            for (std::size_t i = 0; i < items.size(); ++i)
                if (items[i].kind == OfferKind::tool) {
                    press(screens::ids::trade_item + int(i));
                    break;
                }
            for (const char digit : std::to_string(trading_board_.suggestion()))
                press(screens::ids::pad_digit(digit - '0'));
            shot("trading-price-pad");
            press(screens::ids::pad_enter);
            shot("trading-board");
            press(screens::ids::trade_offer);
            shot("trading-detail");
            press(screens::ids::trade_detail_exit);
            press(screens::ids::trade_sale);
            press(screens::ids::trade_category + 1);
            shot("trading-properties");
            close_trading();
        }
        // Synthetic decisions: the real session never pauses here on demand.
        const auto slot = session_->world().header.current_player;
        const auto render = [&](const World &world, const GameDecision &decision) {
            buttons_.clear();
            canvas_ = graphics::make_canvas(640, 480, ui::color(7, 30, 55));
            frame_.ticks = SDL_GetTicks();
            draw_board();
            buttons_.clear();
            auto host = make_host();
            const screens::BankActions none{[](std::int32_t, std::int32_t) { return false; }};
            if (decision.kind == DecisionKind::bank)
                bank_screen_.draw(host, world, decision, pad_, none);
            else if (decision.kind == DecisionKind::lottery)
                screens::draw_lottery(host, world, decision, [](std::size_t) {});
            else if (decision.kind == DecisionKind::auction)
                auction_room_.draw(host, world, decision, [](std::size_t) {});
            else
                screens::draw_primary_stock(host, world, decision, pad_, none);
            if (pad_.active()) {
                buttons_.clear();
                pad_.draw(host);
            }
        };
        const auto decision_shot = [&](const World &world, const GameDecision &decision, const std::string &name) {
            render(world, decision);
            graphics::write_ppm(canvas_, options_.gallery / (name + ".ppm"));
        };
        GameDecision bank{DecisionKind::bank, slot, 0, 0, "", {{"存款", 1}, {"提款", 2}, {"贷款", 3}, {"离开", 0}}};
        const auto &world = session_->world();
        const std::function<void()> redraw_bank = [&] { render(session_->world(), bank); };
        decision_shot(world, bank, "bank-atm");
        press(screens::ids::atm_deposit, redraw_bank);
        press(screens::ids::atm_digit(3), redraw_bank);
        press(screens::ids::atm_digit(0), redraw_bank);
        press(screens::ids::atm_digit(0), redraw_bank);
        press(screens::ids::atm_digit(0), redraw_bank);
        point(260, 343, true); // holding MAX
        decision_shot(world, bank, "bank-atm-typed");
        point(-1, -1);
        press(screens::ids::atm_exit, redraw_bank);
        decision_shot(world, bank, "bank-desk");
        press(screens::ids::desk_loan, redraw_bank);
        press(screens::ids::pad_digit(2), redraw_bank);
        press(screens::ids::pad_digit(0), redraw_bank);
        decision_shot(world, bank, "bank-desk-pad");
        pad_.close();
        // Director's office: make the active player the owner of a bank building.
        World owner = world;
        auto &player = owner.players.at(slot).state;
        for (std::size_t node = 1; node < owner.nodes.size(); ++node) {
            const auto code = owner.nodes[node].type_and_index;
            if (code > 6000 && code < 8000 && std::size_t(code - 6000) < owner.commercials.size()) {
                player.node_id = static_cast<std::uint16_t>(node);
                owner.commercials[code - 6000].owner_code = static_cast<std::uint8_t>(slot + 1);
                break;
            }
        }
        player.loan = 50000;
        owner.players.at(slot).status.loan_due_date = calendar::pack(calendar::add_days(
            {owner.header.date.year, owner.header.date.month, owner.header.date.day}, 40));
        bank.options = {{"存款", 1}, {"提款", 2}, {"贷款", 3}, {"还贷", 4}, {"银行融资", 5}, {"离开", 0}};
        bank_screen_.reset();
        press(screens::ids::atm_exit, [&] { render(owner, bank); });
        const std::function<void()> redraw_owner = [&] { render(owner, bank); };
        decision_shot(owner, bank, "bank-desk-owner");
        press(screens::ids::desk_window, redraw_owner);
        decision_shot(owner, bank, "bank-director");
        bank_screen_.reset();
        GameDecision primary{DecisionKind::primary_stock, slot, 1, 52, "", {{"认购", 1}, {"放弃", 0}}, 380};
        decision_shot(world, primary, "primary-stock");
        pad_.close();
        GameDecision lottery{DecisionKind::lottery, slot, 0, 1000, "", {}};
        for (std::int32_t n = 1; n <= 36; ++n)
            if (n % 7 != 3)
                lottery.options.push_back({std::to_string(n), n});
        lottery.options.push_back({"离开", 0});
        point(30 + 64 * 4 + 20, 271 + 48 + 20);
        decision_shot(world, lottery, "lottery");
        point(-1, -1);
        { // after a pick: ring, wink and 拜拜！祝您中獎！ until the line is said
            screens::LotteryFarewell farewell;
            for (std::size_t i = 0; i < lottery.options.size(); ++i)
                if (lottery.options[i].value == 14)
                    farewell = {i, 13, SDL_GetTicks(), 0};
            buttons_.clear();
            canvas_ = graphics::make_canvas(640, 480, 0);
            frame_.ticks = SDL_GetTicks();
            auto host = make_host();
            screens::draw_lottery(host, world, lottery, [](std::size_t) {}, &farewell);
            graphics::write_ppm(canvas_, options_.gallery / "lottery-farewell.ppm");
        }
        GameDecision auction{DecisionKind::auction, slot, 2001, 2500, "", {}};
        for (std::int32_t action = 0; action <= 6; ++action)
            auction.options.push_back({"", action});
        point(406, 229, true);
        decision_shot(world, auction, "auction");
        point(-1, -1);
        // AUCTION poses: one bidder cannot bid (28+3c); the human's +1000 bid animating (27+3c, starburst).
        auction.inactive_players = static_cast<std::uint8_t>(1U << ((slot + 1) % world.players.size()));
        auction_room_.preview_bid(auction, 3, 3, auction.amount + 1000, SDL_GetTicks() - 450);
        decision_shot(world, auction, "auction-bid");
        auction_room_.reset();
        for (const unsigned at : {300U, 2500U}) { // sold sequence: gavel + price, then congratulations
            auction_room_.start_sold((slot + 2) % world.players.size(), 3500, 2001);
            buttons_.clear();
            canvas_ = graphics::make_canvas(640, 480, ui::color(7, 30, 55));
            frame_.ticks = SDL_GetTicks();
            draw_board();
            auto host = make_host();
            auction_room_.draw_sold(host, world); // stamps the start
            frame_.ticks += at;
            auction_room_.draw_sold(host, world);
            graphics::write_ppm(canvas_, options_.gallery / ("auction-sold-" + std::to_string(at) + ".ppm"));
            auction_room_.reset();
        }
        // 託管AI window: the human selected with 託管 on, cards on, 大老奸, 70% cash / 30% stock.
        if (ai_settings_.open(*session_, slot) && ai_settings_.edited(slot)) {
            auto edited = *ai_settings_.edited(slot);
            screen_ = Screen::ai_settings;
            last_screen_ = Screen::ai_settings;
            press(screens::ids::ai_tab + int(slot)); // the selected tab toggles 託管
            press(screens::ids::ai_tools);
            press(screens::ids::ai_personality + 2);
            for (int i = edited.cash_ratio; i < 70; i += 10)
                press(screens::ids::ai_cash_up);
            for (int i = edited.stock_ratio; i < 30; i += 10)
                press(screens::ids::ai_stock_up);
            shot("ai-settings");
            point(499, 187, true); // holding 確定
            shot("ai-settings-pressed");
            point(-1, -1);
            screen_ = Screen::game;
        }
        gallery_help_endgame(shot, point); // HELP / END SCREENS
        { // Festival calendar pictures (Christmas) on the main screen and the bank side panel.
            auto christmas = session_->world();
            christmas.header.date = {25, 12, 2026};
            install_session(std::move(christmas), options_.seed);
            main_screen_.cycle_view_mode();
            main_screen_.cycle_view_mode(); // view 0: info panel + calendar
            shot("calendar-festival");
            main_screen_.cycle_view_mode(); // back to the default view
            bank_screen_.reset();
            const std::function<void()> redraw_festival = [&] { render(session_->world(), bank); };
            redraw_festival();
            press(screens::ids::atm_exit, redraw_festival);
            press(screens::ids::desk_loan, redraw_festival); // the loan pad shows the side-panel calendar
            decision_shot(session_->world(), bank, "bank-festival");
            pad_.close();
        }
        session_.reset();
        screen_ = Screen::settings;
        shot("options-menu");
        press(screens::ids::options_big[2]); // HELP: 遊戲說明 out of a game, centred
        shot("help-options");
        help_.close();
        std::cout << "Gallery written to " << options_.gallery << '\n';
        return 0;
    }
    // HELP / END SCREENS gallery shots over the current board; endgame time advances 50 ms per step.
    template <typename Shot, typename Point> void gallery_help_endgame(const Shot &shot, const Point &point) {
        screen_ = Screen::game;
        help_.open(20, 60);
        shot("help-toolbar");
        press(screens::ids::help_category + 3); // 房地產 / 公司企業: three pages
        press(screens::ids::help_page_next);
        shot("help-paged");
        press(screens::ids::help_category + 6); // 卡片: 30 topics
        press(screens::ids::help_list_down);
        press(screens::ids::help_topic + 1);
        point(320, 64, true); // holding the next-page arrow
        shot("help-cards-scrolled");
        point(-1, -1);
        help_.close();
        std::uint64_t ticks = SDL_GetTicks();
        const auto run = [&](unsigned steps, bool redraw_board) {
            auto host = make_host();
            host.instant = false;
            for (unsigned i = 0; i < steps; ++i) {
                if (redraw_board)
                    draw();
                ticks += 50;
                frame_.ticks = ticks;
                buttons_.clear();
                endgame_.draw(host, false, {});
            }
        };
        const auto save_shot = [&](const std::string &name) {
            graphics::write_ppm(canvas_, options_.gallery / (name + ".ppm"));
        };
        endgame_.open_play_again();
        run(2, true);
        save_shot("endgame-play-again");
        run(70, true);
        save_shot("endgame-play-again-later");
        for (const auto &button : buttons_)
            if (button.id == screens::ids::endgame_play_again)
                button.action();
        run(2, true);
        save_shot("endgame-movie-early");
        run(8, true);
        save_shot("endgame-movie-late");
        endgame_.open_stage_clear({0, 1, 10, 0b0011, true, 7});
        run(8, false);
        save_shot("endgame-stage-clear-drop");
        point(330, 385); // over JAPAN (uncleared): highlighted
        run(40, false);
        save_shot("endgame-stage-clear-hover");
        for (const auto &button : buttons_)
            if (button.id == screens::ids::endgame_plate + 2)
                button.action();
        run(3, false);
        save_shot("endgame-stage-clear-picked");
        point(-1, -1);
        endgame_.open_stage_clear({1, 2, 3, 0b0100, false, 9});
        run(30, false);
        save_shot("endgame-stage-clear-multi");
        endgame_.close();
    }
    // Dev: all 12 characters in three games of four. In each shot the info-panel player owns
    // every land (levels 0..5), so the name bar, plot outlines/flags and minimap dots sit side by side.
    void colour_gallery() {
        fs::create_directories(options_.output / "colours");
        NewGameOptions configuration;
        configuration.random_seed = options_.seed;
        for (std::uint8_t group = 0; group < 3; ++group) {
            configuration.players.clear();
            for (std::uint8_t i = 0; i < 4; ++i)
                configuration.players.push_back(
                    {static_cast<std::uint8_t>(group * 4 + i), i ? std::uint8_t(2) : std::uint8_t(1), 0, 0});
            const auto world = World::from_map_resource(artwork_.archive('m').decode(1), configuration);
            for (std::uint8_t slot = 0; slot < 4; ++slot) {
                auto copy = world;
                copy.header.current_player = slot;
                for (std::size_t id = 1; id < copy.lands.size(); ++id) {
                    copy.lands[id].economy.owner_code = static_cast<std::uint8_t>(slot + 1);
                    copy.lands[id].economy.level = static_cast<std::uint8_t>(id % 6);
                }
                install_session(std::move(copy), options_.seed);
                screen_ = Screen::game;
                buttons_.clear();
                canvas_ = graphics::make_canvas(640, 480, ui::color(7, 30, 55));
                draw_board();
                graphics::write_ppm(canvas_, options_.output / "colours" /
                                                 ("character-" + std::to_string(group * 4 + slot) + ".ppm"));
            }
        }
        // The same plots owned by 沙隆巴斯 (slot 1) while he is in hospital: not drawn, colour kept.
        configuration.players = {{0, 1, 0, 0}, {1, 2, 0, 0}, {2, 2, 0, 0}, {3, 2, 0, 0}};
        auto world = World::from_map_resource(artwork_.archive('m').decode(1), configuration);
        for (std::size_t id = 1; id < world.lands.size(); ++id) {
            world.lands[id].economy.owner_code = 2;
            world.lands[id].economy.level = static_cast<std::uint8_t>(id % 6);
        }
        world.players[1].status.days_in_hospital = 3;
        install_session(std::move(world), options_.seed);
        screen_ = Screen::game;
        buttons_.clear();
        canvas_ = graphics::make_canvas(640, 480, ui::color(7, 30, 55));
        draw_board();
        graphics::write_ppm(canvas_, options_.output / "colours" / "owner-in-hospital.ppm");
        write_save(options_.output / "colours" / "owner-in-hospital.DAT");
    }
    // Renders sample overlays over the current board for visual review (headless --output only).
    void dump_overlay_gallery() {
        const auto &world = session_->world();
        const auto shoot = [this](const std::string &name, std::optional<messages::Item> item) {
            messages_.clear();
            if (item)
                messages_.push(std::move(*item));
            gallery_ = true;
            draw();
            gallery_ = false;
            graphics::write_ppm(canvas_, options_.output / ("gallery-" + name + ".ppm"));
            messages_.clear();
        };
        const auto event = [&](Announcement kind, std::uint8_t id = 0) {
            GameEvent sample{GameEventKind::notice, 0, 0, 0, {}};
            sample.announcement = kind;
            sample.announcement_id = id;
            return sample;
        };
        shoot("bubble-danny", messages::speech_bubble(10, 0, 0));
        shoot("bubble-john-3lines", messages::speech_bubble(0, 24, 0));
        shoot("bubble-sara-mood1", messages::speech_bubble(5, 18, 0));
        shoot("bubble-beibei-pictogram", messages::speech_bubble(11, 3, 0));
        // Card (slot row*30 + card-1) and tool lines with their call-site moods.
        shoot("bubble-card-monster-user", messages::item_speech_bubble(0, SpeechTable::card, 10, 0, 0));
        shoot("bubble-card-monster-owner", messages::item_speech_bubble(3, SpeechTable::card, 70, 1, 0));
        shoot("bubble-card-beibei-demolish", messages::item_speech_bubble(11, SpeechTable::card, 11, 0, 0));
        shoot("bubble-tool-missile", messages::item_speech_bubble(6, SpeechTable::tool, 6, 0, 0));
        shoot("bubble-tool-mine-hit", messages::item_speech_bubble(9, SpeechTable::tool, 15, 1, 0));
        std::uint16_t land = 1;
        while (land + 1U < world.lands.size() && world.lands[land].economy.street_name.empty())
            ++land;
        auto toll = event(Announcement::toll);
        toll.property = static_cast<std::uint16_t>(2000 + land);
        toll.subject = 1;
        toll.amount = 1200;
        shoot("box-toll", messages::from_event(toll, world));
        toll.second_subject = 2;
        shoot("box-toll-allied", messages::from_event(toll, world));
        shoot("box-cash", messages::from_event(event(Announcement::cash_insufficient), world));
        auto coupon = event(Announcement::point_coupon);
        coupon.amount = 50;
        shoot("box-coupon", messages::from_event(coupon, world));
        auto mini_game = event(Announcement::mini_game_points);
        mini_game.amount = 63;
        shoot("box-mini-game", messages::from_event(mini_game, world));
        auto absent = event(Announcement::absent_status, 2);
        absent.value = 3;
        shoot("box-prison", messages::from_event(absent, world));
        for (const std::uint8_t id : {1, 5, 11, 29, 33}) {
            auto news = event(Announcement::news, id);
            news.property = static_cast<std::uint16_t>(2000 + land);
            news.players = 0b1011;
            news.player_amounts = {2940, 4185, 0, 6205};
            news.subject = 1;
            news.company = 1;
            news.value = 3;
            shoot("news-" + std::to_string(id), messages::from_event(news, world));
        }
        for (const std::uint8_t id : {0, 20, 33}) {
            auto fortune = event(Announcement::fortune, id);
            fortune.value = id == 20 ? 1000 : 3;
            shoot("fortune-" + std::to_string(id), messages::from_event(fortune, world));
        }
        shoot("god-1", messages::from_event(event(Announcement::god_arrival, 1), world));
        shoot("god-12", messages::from_event(event(Announcement::god_arrival, 12), world));
        GameDecision buy{DecisionKind::buy_land, 0, land, 800, {}, {{"购买", 1}, {"放弃", 0}}};
        for (const int hover : {0, 1, 2}) {
            messages_.clear();
            gallery_ = true;
            draw();
            gallery_ = false;
            buttons_.clear();
            frame_.pointer_x = hover == 1 ? 190 : hover == 2 ? 250 : -1;
            frame_.pointer_y = hover ? 320 : -1;
            messages::draw_yes_no(frame_, messages::yes_no_question(buy, world), 210, [] {}, 211, [] {});
            frame_.pointer_x = frame_.pointer_y = -1;
            graphics::write_ppm(canvas_, options_.output / ("gallery-yesno-" + std::to_string(hover) + ".ppm"));
        }
        dump_rules_text_gallery(land);
        buttons_.clear();
        draw();
    }
    // Review shots for the Traditional prompts and the map-tile popup (gallery-rules-*.ppm).
    void dump_rules_text_gallery(std::uint16_t land) {
        const auto &world = session_->world();
        const auto base = [this] {
            messages_.clear();
            gallery_ = true;
            draw();
            gallery_ = false;
            buttons_.clear();
        };
        const auto save = [this](const std::string &name) {
            graphics::write_ppm(canvas_, options_.output / ("gallery-rules-" + name + ".ppm"));
        };
        const auto slot = world.header.current_player;
        GameDecision free_card{DecisionKind::card_target, slot, 0, 1200, "是否使用免費卡？",
                               {{"使用免費卡", 1}, {"保留卡片", 0}}};
        base();
        messages::draw_yes_no(frame_, messages::yes_no_question(free_card, world), 210, [] {}, 211, [] {});
        save("free-card");
        const auto other = (slot + 1) % world.players.size();
        GameDecision frame_card{DecisionKind::card_target, slot, 0, 0, "", {}};
        frame_card.options.push_back({"", std::int32_t(other), std::nullopt,
                                      CardTarget{CardTargetKind::actor, static_cast<std::uint16_t>(other)}});
        frame_card.options.push_back({"保留卡片", -1});
        base();
        messages::draw_yes_no(frame_, messages::yes_no_question(frame_card, world), 210, [] {}, 211, [] {});
        save("frame-card-one");
        frame_card.prompt = "請選擇嫁禍對象...";
        frame_card.options.insert(frame_card.options.begin() + 1,
                                  {"", std::int32_t((slot + 2) % world.players.size()), std::nullopt,
                                   CardTarget{CardTargetKind::actor,
                                              static_cast<std::uint16_t>((slot + 2) % world.players.size())}});
        base();
        draw_choice_dialog(messages::localized(frame_card, world));
        save("frame-card-several");
        { // FACILITY: build (no cancel) and 改建卡 (with 取消), idle and with a slot selected
            const GameDecision build{DecisionKind::construct_facility, slot, 1, 0, "請選擇設施類別",
                                     {{"公園", 0}, {"旅館", 1}, {"百貨公司", 2}, {"加油站", 3}, {"研究所", 4}}};
            const GameDecision rebuild{DecisionKind::action_target, slot, 0, 0, "請選擇設施類別",
                                       {{"公園", 0}, {"旅館", 1}, {"百貨公司", 2}, {"加油站", 3}, {"研究所", 4},
                                        {"取消", -1}}};
            auto host = make_host();
            for (const auto &[name, decision, selected] :
                 {std::tuple{"facility-type", &build, std::optional<int>{}},
                  std::tuple{"facility-type-selected", &build, std::optional<int>{2}},
                  std::tuple{"facility-rebuild", &rebuild, std::optional<int>{}},
                  std::tuple{"facility-rebuild-selected", &rebuild, std::optional<int>{0}}}) {
                base();
                facility_picker_.select(selected, name);
                facility_picker_.draw(host, *decision, name, [](std::size_t) {});
                save(name);
            }
            facility_picker_.select(std::nullopt, {});
        }
        base();
        draw_choice_dialog(GameDecision{DecisionKind::action_target, slot, 0, 0, "選擇點數",
                                        {{"1點", 1}, {"2點", 2}, {"3點", 3}, {"4點", 4}, {"5點", 5}, {"6點", 6},
                                         {"取消", 0}}});
        save("remote-dice");
        base();
        draw_choice_dialog(GameDecision{DecisionKind::notice, slot, 0, 0, "本次事件略過。", {{"繼續", 0}}});
        save("notice");
        GameDecision red{DecisionKind::action_target, slot, 0, 0, "紅卡：選擇目標", {}};
        for (std::uint16_t stock = 0; stock < 6; ++stock)
            red.options.push_back({"股票" + std::to_string(stock + 1), stock, std::nullopt,
                                   CardTarget{CardTargetKind::stock, stock}});
        red.options.push_back({"取消", -1});
        base();
        draw_choice_dialog(messages::localized(red, world));
        save("red-card");
        // Construction company target panel: real street/facility names, 取消 button.
        GameDecision build{DecisionKind::property_target, slot, 0, 0, "請選擇欲加蓋地點", {}};
        for (std::size_t i = 1; i < world.lands.size() && build.options.size() < 12; ++i)
            build.options.push_back({"房產" + std::to_string(i), std::int32_t(2000 + i),
                                     DecisionLocation{world.lands[i].x, world.lands[i].y},
                                     CardTarget{CardTargetKind::property, static_cast<std::uint16_t>(2000 + i)}});
        build.options.push_back({"取消", 0});
        base();
        selected_target_.reset();
        draw_map_decision(build);
        save("construction-targets");
        buttons_.clear();
        // Map-tile popups: tap points are the projected anchors, hit-tested through instance_at.
        const auto shoot_popup = [&](const std::string &name, const World &shown, std::uint16_t code, int dy) {
            board::Point world_point{};
            if (code > 2000 && code < 4000)
                world_point = {shown.lands[code - 2000].x, shown.lands[code - 2000].y};
            else if (code > 4000 && code < 6000)
                world_point = {shown.facilities[code - 4000].x, shown.facilities[code - 4000].y};
            else if (code > 6000 && code < 8000)
                world_point = {shown.commercials[code - 6000].x, shown.commercials[code - 6000].y};
            else if (code & 0x8000U)
                world_point = {shown.players[std::size_t(std::countr_zero(unsigned(code & 0xffU)))].state.x,
                               shown.players[std::size_t(std::countr_zero(unsigned(code & 0xffU)))].state.y};
            else
                world_point = {shown.nodes[code].x, shown.nodes[code].y};
            base();
            const auto at = board::project(world_point, current_camera_);
            if (!at)
                return;
            const int x = at->x, y = at->y + dy;
            const auto hit = native::tile_popup::instance_at(shown, current_camera_, x, y);
            native::tile_popup::draw(frame_, shown, {x, y, hit ? hit : code});
            if (hit != code)
                std::cerr << "tile popup " << name << ": tap hit " << hit << " instead of " << code << '\n';
            save("popup-" + name);
        };
        // Pick properties near the current camera so the shots show them on screen.
        const auto visible = [&](int wx, int wy) {
            const auto p = board::project({wx, wy}, current_camera_);
            return p && p->x > 30 && p->x < 410 && p->y > 80 && p->y < 450;
        };
        auto shown = world;
        std::vector<std::uint16_t> lands;
        for (std::size_t i = 1; i < shown.lands.size(); ++i)
            if (visible(shown.lands[i].x, shown.lands[i].y))
                lands.push_back(static_cast<std::uint16_t>(i));
        if (lands.empty())
            lands.push_back(land);
        shoot_popup("land-unowned", shown, static_cast<std::uint16_t>(2000 + lands[0]), 0);
        auto &owned = shown.lands[lands[0]].economy;
        owned.owner_code = 1;
        owned.level = 3;
        shoot_popup("land-owned-level3", shown, static_cast<std::uint16_t>(2000 + lands[0]), -20);
        owned.chain_store = 1;
        owned.level = 1;
        shoot_popup("land-chain", shown, static_cast<std::uint16_t>(2000 + lands[0]), -10);
        for (std::size_t i = 1; i < shown.facilities.size(); ++i)
            if (visible(shown.facilities[i].x, shown.facilities[i].y)) {
                shoot_popup("facility", shown, static_cast<std::uint16_t>(4000 + i), 0);
                break;
            }
        for (std::size_t i = 1; i < shown.commercials.size(); ++i)
            if (visible(shown.commercials[i].x, shown.commercials[i].y)) {
                shoot_popup("company", shown, static_cast<std::uint16_t>(6000 + i), -20);
                break;
            }
        shoot_popup("player", shown, static_cast<std::uint16_t>(0x8000U | (1U << slot)), -20);
        for (std::size_t i = 1; i < shown.nodes.size(); ++i)
            if (!shown.nodes[i].name.empty() && visible(shown.nodes[i].x, shown.nodes[i].y) &&
                !(shown.nodes[i].flags & 0x00ffff00U)) {
                shoot_popup("node", shown, static_cast<std::uint16_t>(i), 0);
                break;
            }
    }
    void finish_test() {
        const bool passed =
            reload_identical_ && reload_random_identical_ &&
            (test_completed_days_ >= options_.auto_rounds || test_finished_) &&
            (!options_.exercise_card || card_ids_used_.contains(options_.exercise_card)) &&
            (!options_.exercise_tool || tool_ids_used_.contains(options_.exercise_tool)) &&
            (!options_.exercise_recovery || (test_invalid_load_stage_ == 4 && test_batched_cancel_)) &&
            (!options_.exercise_trading || (offers_posted_ && offers_cancelled_ && offers_bought_)) &&
            dice_by_player_[0] > 0 && dice_by_player_[1] + dice_by_player_[2] + dice_by_player_[3] > 0 &&
            event_counts_[GameEventKind::moved] > 0 && saves_ && loads_ && input_events_ &&
            unsupported_.empty();
        std::ofstream report(options_.output / "report.json");
        report << "{\"schema\":1,\"target\":\"native\",\"status\":\"" << (passed ? "passed" : "failed")
               << "\",\"native_gameplay_verified\":" << (passed ? "true" : "false")
               << ",\"wine_runtime\":false,\"desktop_input\":false,\"video_driver\":"
               << quoted(SDL_GetCurrentVideoDriver())
               << ",\"renderer\":" << quoted(SDL_GetRendererName(renderer_.get()))
               << ",\"requested_days\":" << options_.auto_rounds
               << ",\"completed_days\":" << test_completed_days_
               << ",\"match_finished\":" << (test_finished_ ? "true" : "false")
               << ",\"seed\":" << options_.seed
               << ",\"initial_game_random_state\":" << initial_random_state_.value_or(0)
               << ",\"map\":" << session_->world().header.stage_id * 4 + session_->world().header.map_id
               << ",\"requested_card\":" << options_.exercise_card
               << ",\"requested_tool\":" << options_.exercise_tool
               << ",\"invalid_load_recovered\":" << (test_invalid_load_stage_ == 4 ? "true" : "false")
               << ",\"offers_posted\":" << offers_posted_ << ",\"offers_cancelled\":" << offers_cancelled_
               << ",\"offers_bought\":" << offers_bought_
               << ",\"batched_cancel_recovered\":" << (test_batched_cancel_ ? "true" : "false")
               << ",\"new_games\":" << new_games_ << ",\"loads\":" << loads_ << ",\"saves\":" << saves_
               << ",\"reload_bytes_identical\":" << (reload_identical_ ? "true" : "false")
               << ",\"reload_random_identical\":" << (reload_random_identical_ ? "true" : "false")
               << ",\"sdl_input_events\":" << input_events_ << ",\"rendered_frames\":" << rendered_frames_
               << ",\"dice_actions\":" << event_counts_[GameEventKind::dice_rolled]
               << ",\"movement_steps\":" << event_counts_[GameEventKind::moved]
               << ",\"land_purchases\":" << event_counts_[GameEventKind::land_purchased]
               << ",\"land_upgrades\":" << event_counts_[GameEventKind::land_upgraded]
               << ",\"rent_payments\":" << event_counts_[GameEventKind::rent_paid]
               << ",\"bank_transfers\":" << event_counts_[GameEventKind::bank_transfer]
               << ",\"cards_used\":" << cards_used_ << ",\"tools_used\":" << tools_used_
               << ",\"stock_buys\":" << stocks_bought_ << ",\"stock_sales\":" << stocks_sold_
               << ",\"dice_selections\":" << dice_selections_ << ",\"audio_driver\":"
               << quoted(SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "")
               << ",\"audio_frames\":" << audio_->statistics().rendered_frames
               << ",\"audio_effects\":" << audio_->statistics().effects_started
               << ",\"audio_voices\":" << audio_->statistics().voices_started
               // AUDIO glitch diagnostics (native_audio.hpp Statistics)
               << ",\"audio_underruns\":" << audio_->statistics().underruns
               << ",\"audio_starved_ms\":" << audio_->statistics().starved_ms
               << ",\"audio_clipped_samples\":" << audio_->statistics().clipped_samples
               << ",\"audio_limited_samples\":" << audio_->statistics().limited_samples
               << ",\"audio_peak\":" << audio_->statistics().peak
               << ",\"audio_max_update_gap_ms\":" << audio_->statistics().max_update_gap_ms
               << ",\"audio_max_render_us\":" << audio_->statistics().max_render_us
               << ",\"music_available\":" << (audio_->music_available() ? "true" : "false")
               << ",\"audio_log\":{";
        for (bool first = true; const auto &[what, count] : audio_log_counts_) {
            report << (first ? "" : ",") << quoted(what) << ':' << count;
            first = false;
        }
        report << "}"
               << ",\"unsupported_events\":[";
        for (std::size_t index = 0; index < unsupported_.size(); ++index) {
            if (index)
                report << ',';
            report << quoted(unsupported_[index]);
        }
        report << "],\"card_ids_used\":[";
        bool comma = false;
        for (auto id : card_ids_used_) {
            if (comma)
                report << ',';
            report << id;
            comma = true;
        }
        report << "],\"tool_ids_used\":[";
        comma = false;
        for (auto id : tool_ids_used_) {
            if (comma)
                report << ',';
            report << id;
            comma = true;
        }
        report << "]}\n";
        std::cout << "Native gameplay report: " << options_.output / "report.json" << '\n';
        running_ = false;
        final_status_ = passed ? 0 : 1;
    }

    Options options_;
    Artwork artwork_;
    ui::Font font_;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window_{nullptr, SDL_DestroyWindow};
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer_{nullptr, SDL_DestroyRenderer};
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture_{nullptr, SDL_DestroyTexture};
    std::unique_ptr<GameSession> session_;
    std::unique_ptr<board::Renderer> board_;
    std::unique_ptr<audio::Audio> audio_;
    native::EffectPlayer effects_{artwork_}; // EFFECTS
    native::IntroPlayer intro_{artwork_};    // new-game parachute intro (screens/intro.hpp)
    Uint64 intro_ticks_ = 0, intro_capture_ = 0;
    unsigned intro_frames_ = 0;
    bool intro_pressed_ = false; // a press began during the intro; its release skips
    facility_ui::Picker facility_picker_; // FACILITY
    spinners::Spinners spinners_{{[this](std::size_t id) { // SPINNERS
                                      if (sound_effects_)
                                          audio_->play_sound(id);
                                  },
                                  [this](std::size_t id) {
                                      if (sound_effects_)
                                          audio_->play_loop(id);
                                  },
                                  [this] { audio_->stop_loop(); }}};
    Uint64 spinner_ticks_ = 0; // SPINNERS: last spinner effect draw time (MONTHLY shares this clock)
    screens::LotteryFarewell lottery_farewell_; // the bar stays up for 拜拜！祝您中獎！ after a pick
    Uint64 stock_card_hold_until_ = 0; // 紅卡 / 黑卡: market page held after the card took effect
    std::size_t stock_card_player_ = 0;
    Uint64 monthly_ticks_ = 0; // MONTHLY: last draw_monthly() time
    native::monthly::MonthlyScreens monthly_{{[this](std::size_t id) { // MONTHLY: Effect.mkf / Speaking.mkf
                                          if (sound_effects_)
                                              audio_->play_sound(id);
                                      },
                                      [this](std::size_t entry) {
                                          return sound_effects_ ? audio_->play_speech(entry) : 0U;
                                      }}};
    minigames::MiniGames minigames_{{[this](std::size_t id) { // MINIGAMES
                                         if (sound_effects_)
                                             audio_->play_sound(id);
                                     },
                                     [this](std::size_t id) {
                                         if (sound_effects_)
                                             audio_->play_loop(id);
                                     },
                                     [this] { audio_->stop_loop(); }}};
    bool interactive_minigames_ = true; // MINIGAMES: Options 動畫過程 (RICH4.CFG[1]), on by default
    unsigned dev_minigame_rounds_ = 0;  // MINIGAMES (dev)
    unsigned test_spinner_frames_ = 0; // SPINNERS (--play-effects): frames shown before the replay STOP
    std::deque<PresentationStep> presentation_; // see observe()
    unsigned hold_ms_ = 0;                       // EffectKind::pause still to wait (game ms)
    std::optional<std::size_t> presented_player_;
    Uint64 effect_ticks_ = 0;                // EFFECTS: last draw_effects() time
    Uint64 effect_capture_ = 0;              // EFFECTS (--play-effects): next frame dump time
    unsigned effect_frames_ = 0;
    messages::Overlay messages_{{[this](std::size_t entry) { return sound_effects_ ? audio_->play_speech(entry) : 0U; },
                                 [this] { audio_->stop_voice(); }}};
    std::map<std::string, unsigned> overlay_captures_;
    bool gallery_ = false;
    graphics::Image canvas_;
    std::vector<Button> buttons_;
    native::Frame frame_{canvas_, artwork_, font_, buttons_};
    native::MainScreen main_screen_{options_.ui_preset};
    Screen screen_ = Screen::menu, last_screen_ = Screen::menu, previous_screen_ = Screen::menu;
    screens::NumberPad pad_;
    screens::BankScreen bank_screen_;
    screens::StockMarket stock_market_;
    screens::MainMenu main_menu_;
    screens::NewGameSetup setup_;
    screens::SaveLoadScreen slots_;
    screens::OptionsScreen options_screen_;
    // END SCREENS: shown once per finished session (endgame_session_); cleared_maps_ is the
    // original 0x4990f0 campaign record of the stage (reset on the main menu, as loc_00401c8c).
    screens::EndgameScreen endgame_;
    const GameSession *endgame_session_ = nullptr;
    std::uint8_t cleared_maps_ = 0;
    unsigned cleared_stage_ = 0;
    unsigned humans_at_start_ = 1; // _num_human_players, counted when a session is installed
    screens::HelpScreen help_; // HELP: modal over any screen
    screens::PlayerInfo info_;
    screens::TradingBoard trading_board_;
    screens::AuctionRoom auction_room_;     // AUCTION: bid poses and the sold sequence
    screens::AiSettingsScreen ai_settings_; // 託管AI window (toolbar icon 2)
    native::items::Screens items_;
    unsigned selected_character_ = 10, selected_map_ = 0, selected_stage_ = 0, rotation_ = 0, speed_ = 1;
    bool running_ = true, bank_done_for_visit_ = false, shop_done_for_visit_ = false,
         sound_effects_ = true;
    std::int32_t bank_amount_ = 1000;
    std::int32_t amount_limit_ = 0;
    std::int32_t *editing_amount_ = nullptr;
    std::string amount_text_;
    std::size_t selected_stock_ = 0;
    std::uint64_t offers_posted_ = 0, offers_cancelled_ = 0, offers_bought_ = 0;
    std::set<std::pair<std::size_t, std::size_t>> test_trade_attempts_;
    bool test_trade_acted_ = false;
    std::optional<std::uint32_t> initial_random_state_;
    board::Camera current_camera_{};
    std::optional<native::tile_popup::Popup> tile_popup_; // shown until the next tap
    std::optional<board::Point> target_center_;
    std::optional<std::size_t> selected_target_;
    std::uint64_t last_autosaved_day_ = UINT64_MAX;
    std::vector<std::string> recent_, unsupported_;
    std::string toast_;
    Uint64 toast_until_ = 0;
    std::string decision_key_;
    std::size_t decision_page_ = 0;
    std::ofstream events_;
    std::ofstream inputs_;
    std::ofstream audio_log_;                            // AUDIO: OUTPUT/audio.jsonl
    std::map<std::string, std::uint64_t> audio_log_counts_; // AUDIO: report "audio_log"
    native::WalkSound walk_sound_;                       // AUDIO
    native::MusicDirector music_director_;               // AUDIO
    std::string voiced_decision_key_;                    // AUDIO: decision whose voices played
    bool auction_open_ = false, bankrupt_music_ = false; // AUDIO
    std::set<std::string> captured_frames_;
    std::map<std::string, unsigned> item_shots_;
    std::map<GameEventKind, std::uint64_t> event_counts_;
    std::array<std::uint64_t, 4> dice_by_player_{};
    std::uint64_t input_events_ = 0, rendered_frames_ = 0, new_games_ = 0, loads_ = 0, saves_ = 0;
    std::uint64_t cards_used_ = 0, tools_used_ = 0, stocks_bought_ = 0, stocks_sold_ = 0;
    std::uint64_t dice_selections_ = 0;
    std::set<unsigned> card_ids_used_, tool_ids_used_;
    unsigned test_inventory_id_ = 0, test_stock_stage_ = 0;
    bool tested_cards_ = false, tested_tools_ = false, tested_dice_ = false, test_inventory_pushed_ = false;
    std::string test_progress_;
    unsigned test_unchanged_frames_ = 0;
    bool load_failed_ = false, test_batched_cancel_ = false;
    bool test_cancel_pending_ = false;
    std::optional<SDL_Event> test_release_;
    std::vector<std::uint8_t> test_cancel_save_;
    std::uint32_t test_cancel_random_ = 0;
    std::uint64_t test_cancel_inputs_ = 0;
    unsigned test_invalid_load_stage_ = 0;
    fs::path test_recovery_path_;
    std::vector<std::uint8_t> test_recovery_save_;
    std::uint32_t test_recovery_random_ = 0;
    unsigned test_stage_ = 0;
    std::uint64_t test_load_count_before_reload_ = 0;
    std::uint64_t test_completed_days_ = 0;
    std::vector<std::uint8_t> expected_reload_;
    bool reload_identical_ = false;
    bool reload_random_identical_ = false;
    std::uint32_t expected_random_ = 0;
    bool test_finished_ = false;
    int final_status_ = 0;
};
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "rich4-native [--assets DIR] [--font FONT] [--load SAVE.DAT] [--save-dir DIR]\n"
                     "  --headless --auto-rounds N --output DIR [--start-new] [--seed N] [--overlay-gallery]\n";
        return 0;
    }
    int status = 0;
    try {
        Application application(arguments(argc, argv));
        try {
            status = application.run();
        } catch (const std::exception &error) {
            try {
                application.write_diagnostic(error.what());
            } catch (const std::exception &diagnostic) {
                std::cerr << "Failure snapshot: " << diagnostic.what() << '\n';
            }
            throw;
        }
    } catch (const std::exception &error) {
        std::cerr << "rich4-native: " << error.what() << '\n';
        status = 1;
    }
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
    // The title screen's EXIT quits: SDL's UIKit delegate deliberately does not exit when main
    // returns (SDL_uikitappdelegate.m postFinishLaunch), which would leave a frozen screen.
    std::exit(status);
#endif
    return status;
}
