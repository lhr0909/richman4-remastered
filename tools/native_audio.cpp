// SPDX-License-Identifier: GPL-3.0-or-later
// Resource selection follows Iru Cai's Rich4 reverse engineering:
// rich4_sound_effect.asm, rich4_media_music.asm, rich4.asm event strings.
#include "native_audio.hpp"

#include "audio_limiter.hpp"

#include "rich4/assets.hpp"

#include <SDL3/SDL.h>
#include <fluidsynth.h>
#ifdef RICH4_HAVE_VORBIS
#include <vorbis/vorbisfile.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace rich4::audio {
namespace {

constexpr int sample_rate = 48000;
constexpr int channels = 2;
constexpr int block_frames = 512;
constexpr int bytes_per_frame = channels * int(sizeof(float));
// Mix levels. Every Effect.mkf / Speaking.mkf clip is normalised to ~0 dBFS (median peak 0.83 /
// 0.99, RMS about -16 dBFS) while the General MIDI render at the old synth gain 0.4 peaks around
// 0.5-0.66 (RMS about -26 dBFS). Summed at unity a voice over music or an effect over a voice
// clipped constantly; these gains keep a single clip plus music under the limiter ceiling and
// keep music within ~1 dB of its old balance against effects and voices.
constexpr float clip_gain = 0.6F;   // effects and voices (-4.4 dB)
constexpr double music_gain = 0.28; // FluidSynth synth.gain (was 0.4)
// The Steam Ogg tracks are mastered near 0 dBFS (RMS about -16 dBFS); -12 dB puts them close to
// the MIDI render's level against effects and voices.
constexpr float ogg_gain = 0.25F;
constexpr SDL_AudioSpec mixer_format{SDL_AUDIO_F32, channels, sample_rate};
constexpr std::array<std::string_view, 8> music_files{"rich08.mid", "rich16.mid", "rich17.mid", "rich18.mid",
                                                      "rich19.mid", "rich20.mid", "rich21.mid", "rich22.mid"};
// ref_0047e793, indexed by Music (silence first).
constexpr std::array<std::string_view, 18> context_files{
    "silence",    "midi01.mid", "midi02.mid", "midi03.mid",   "midi04.mid",   "midi05.mid",
    "midi06.mid", "midi07.mid", "midi08.mid", "midi09.mid",   "midi10.mid",   "midi11.mid",
    "midi12.mid", "midi13.mid", "midi14-1.mid", "midi14-2.mid", "midi15.mid", "midi16.mid"};
constexpr std::array<std::string_view, std::size_t(Loop::count)> loop_names{"spinner", "walk", "bomb"};

// The Steam release replaces the MIDIs with CD-audio tracks. Its Midi.txt lists the 25 MIDIs in
// track order (track 1 is the data track): the playlist, then the context MIDIs, so rich08.mid is
// track02.ogg and midi16.mid is track26.ogg.
std::string ogg_name(std::string_view midi) {
    int track = 2;
    for (const auto file : music_files) {
        if (file == midi)
            break;
        ++track;
    }
    if (track == 2 + int(music_files.size())) {
        for (const auto file : context_files) {
            if (file == "silence")
                continue;
            if (file == midi)
                break;
            ++track;
        }
    }
    char name[16];
    std::snprintf(name, sizeof(name), "track%02d.ogg", track);
    return name;
}

std::filesystem::path asset_path(const std::filesystem::path &directory, std::string_view name) {
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        auto candidate = entry.path().filename().string();
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char value) { return char(std::tolower(value)); });
        if (candidate == name && entry.is_regular_file()) {
            return entry.path();
        }
    }
    throw assets::AssetError("missing audio asset: " + (directory / name).string());
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open MIDI: " + path.string());
    }
    const auto length = input.tellg();
    if (length < 0 || length > 16 * 1024 * 1024) {
        throw std::runtime_error("invalid MIDI size: " + path.string());
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(result.data()), length)) {
        throw std::runtime_error("truncated MIDI: " + path.string());
    }
    return result;
}

std::runtime_error sdl_error(std::string_view operation) {
    return std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

struct AudioSubsystem {
    AudioSubsystem() {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            throw sdl_error("initialize SDL audio");
        }
    }
    ~AudioSubsystem() {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
};

struct SdlFree {
    void operator()(void *pointer) const {
        SDL_free(pointer);
    }
};

std::vector<float> decode_wave(std::span<const std::uint8_t> bytes) {
    auto *input = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (!input) {
        throw sdl_error("open WAV memory");
    }
    SDL_AudioSpec source_format{};
    Uint8 *wav_pointer = nullptr;
    Uint32 wav_length = 0;
    if (!SDL_LoadWAV_IO(input, true, &source_format, &wav_pointer, &wav_length)) {
        throw sdl_error("decode original WAV");
    }
    std::unique_ptr<Uint8, SdlFree> wav(wav_pointer);
    if (wav_length > std::uint32_t(std::numeric_limits<int>::max())) {
        throw assets::AssetError("WAV exceeds SDL conversion length range");
    }
    Uint8 *converted_pointer = nullptr;
    int converted_length = 0;
    if (!SDL_ConvertAudioSamples(&source_format, wav.get(), static_cast<int>(wav_length), &mixer_format,
                                 &converted_pointer, &converted_length)) {
        throw sdl_error("convert original WAV to mixer format");
    }
    std::unique_ptr<Uint8, SdlFree> converted(converted_pointer);
    if (converted_length <= 0 || converted_length % bytes_per_frame != 0) {
        throw assets::AssetError("WAV conversion returned an invalid stereo sample count");
    }
    std::vector<float> result(static_cast<std::size_t>(converted_length) / sizeof(float));
    std::memcpy(result.data(), converted.get(), static_cast<std::size_t>(converted_length));
    return result;
}

#ifdef RICH4_HAVE_VORBIS
// Streams one Ogg Vorbis file, resampled to the mixer format. Caller holds the audio mutex.
class OggTrack {
  public:
    OggTrack() = default;
    OggTrack(const OggTrack &) = delete;
    OggTrack &operator=(const OggTrack &) = delete;
    ~OggTrack() {
        close();
    }

    // Opens `path` at PCM frame `frame`; returns an error message instead of throwing because the
    // audio thread calls it when the playlist advances.
    std::string open(const std::filesystem::path &path, std::int64_t frame) {
        close();
        if (ov_fopen(path.string().c_str(), &file_) != 0)
            return "cannot open Ogg music " + path.string();
        open_ = true;
        const auto *info = ov_info(&file_, -1);
        const SDL_AudioSpec source{SDL_AUDIO_F32, info->channels, int(info->rate)};
        resampler_.reset(SDL_CreateAudioStream(&source, &mixer_format));
        if (!resampler_) {
            close();
            return std::string("create Ogg resampler: ") + SDL_GetError();
        }
        channels_ = info->channels;
        interleaved_.resize(std::size_t(read_frames) * std::size_t(channels_));
        if (frame > 0)
            ov_pcm_seek(&file_, frame);
        return {};
    }
    void close() {
        resampler_.reset();
        if (open_)
            ov_clear(&file_);
        open_ = false;
        ended_ = false;
    }
    bool is_open() const {
        return open_;
    }
    std::int64_t position() {
        return open_ ? ov_pcm_tell(&file_) : 0;
    }
    // Adds up to output.size() samples times `gain` to `output`. Returns false once the file has
    // ended and every resampled sample was played; `loop` restarts it instead.
    bool mix(std::span<float> output, float gain, bool loop) {
        const int wanted = int(output.size() * sizeof(float));
        while (!ended_ && SDL_GetAudioStreamAvailable(resampler_.get()) < wanted) {
            float **pcm = nullptr;
            int section = 0;
            const long frames = ov_read_float(&file_, &pcm, read_frames, &section);
            if (frames == OV_HOLE)
                continue; // recoverable gap in the stream
            if (frames <= 0) {
                if (frames == 0 && loop && ov_pcm_seek(&file_, 0) == 0)
                    continue;
                ended_ = true;
                SDL_FlushAudioStream(resampler_.get());
                break;
            }
            for (long i = 0; i < frames; ++i)
                for (int c = 0; c < channels_; ++c)
                    interleaved_[std::size_t(i * channels_ + c)] = pcm[c][i];
            SDL_PutAudioStreamData(resampler_.get(), interleaved_.data(), int(frames * channels_ * sizeof(float)));
        }
        const int got = SDL_GetAudioStreamData(resampler_.get(), scratch_.data(),
                                               std::min(wanted, int(scratch_.size() * sizeof(float))));
        for (int i = 0; i < got / int(sizeof(float)); ++i)
            output[std::size_t(i)] += scratch_[std::size_t(i)] * gain;
        return !ended_ || SDL_GetAudioStreamAvailable(resampler_.get()) > 0;
    }

  private:
    static constexpr int read_frames = 1024;
    OggVorbis_File file_{};
    bool open_{};
    bool ended_{};
    int channels_{};
    std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)> resampler_{nullptr, SDL_DestroyAudioStream};
    std::vector<float> interleaved_;
    std::array<float, block_frames * channels> scratch_{};
};
#else
// Built without libvorbisfile (phone builds): Ogg music is never discovered.
class OggTrack {
  public:
    std::string open(const std::filesystem::path &, std::int64_t) {
        return "built without Ogg Vorbis support";
    }
    void close() {}
    bool is_open() const {
        return false;
    }
    std::int64_t position() {
        return 0;
    }
    bool mix(std::span<float>, float, bool) {
        return false;
    }
};
#endif

} // namespace

std::string_view music_file(Music music) noexcept {
    const auto index = static_cast<std::size_t>(music);
    return index < context_files.size() ? context_files[index] : "silence";
}

// Threading model (see native_audio.hpp):
//  * SDL's audio device thread calls feed() whenever the device needs samples and mixes exactly
//    that much, so a stalled main thread (archive open, FLC decode, map load, slow software
//    canvas) can no longer starve the device.
//  * Everything slow -- archive IO, WAV decode, MIDI file reads, soundbank loading -- happens on
//    the main thread (or the startup archive loader) *outside* `mutex`; the main thread then
//    takes `mutex` only to swap pointers / small structs. The exception is Ogg music (Steam
//    release): it is streamed from disk on the audio thread, in small reads.
//  * Fields marked [shared] are guarded by `mutex`. Fields marked [main] are only touched by the
//    main thread. [const] fields are fixed after construction.
//  * The audio thread never throws and never calls the trace; its log lines are queued and
//    handed to the trace by update() on the main thread.
struct Audio::Impl {
    // [const]
    bool enabled{};
    std::filesystem::path directory;
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> midi_cache; // every original MIDI, read at startup
    std::map<std::string, std::filesystem::path, std::less<>> ogg_paths; // Steam Ogg tracks, keyed by MIDI name
    // [main]
    Trace trace;
    std::array<std::optional<std::size_t>, std::size_t(Loop::count)> loops{};
    struct Saved {
        std::optional<Music> context; // nullopt = board playlist track
        unsigned track{1};
        int tick{};
    };
    std::vector<Saved> music_stack; // pushed contexts save what was under them
    std::optional<assets::Archive> effects;
    std::optional<assets::Archive> voices;
    std::future<void> archive_loader; // opens Effect.mkf/Speaking.mkf off the main thread at startup
    std::string archive_error;        // why the loader failed; every later clip request rethrows it
    // Decoded clips; std::map nodes never move, so the audio thread may keep `samples` pointers.
    std::map<std::pair<bool, std::size_t>, std::vector<float>> samples;
    std::uint64_t last_update_ns{};
    bool error_reported{};
    std::string diagnostic;

    // [shared] -- everything below is guarded by `mutex`
    mutable std::mutex mutex;
    bool music_on{true};
    unsigned track{1};
    std::optional<Music> context; // what the MIDI player plays: a context (looped) or playlist `track`
    bool player_stale{};          // music was off when the track changed: (re)start when on
    int resume_tick{};            // position to seek to when a stale track is started
    Statistics stats;
    std::unique_ptr<fluid_settings_t, decltype(&delete_fluid_settings)> settings{nullptr,
                                                                                 delete_fluid_settings};
    std::unique_ptr<fluid_synth_t, decltype(&delete_fluid_synth)> synth{nullptr, delete_fluid_synth};
    std::unique_ptr<fluid_player_t, decltype(&delete_fluid_player)> player{nullptr, delete_fluid_player};
    OggTrack ogg; // used instead of `player` when the music is Ogg (then `synth` stays null)
    struct Clip {
        const std::vector<float> *samples;
        std::size_t position{};
        bool voice{};
        bool loop{}; // play_loop(): restarts at the end until stop_loop()
        Loop channel{Loop::spinner};
    };
    std::vector<Clip> clips;
    struct Queued {
        std::size_t index;
        const std::vector<float> *samples; // decoded by queue_speech on the main thread
    };
    std::vector<Queued> speech_queue;
    std::vector<std::pair<std::string_view, std::string>> pending_log; // audio-thread trace lines
    std::string render_error;
    Limiter limiter{float(sample_rate)};
    // Dev: RICH4_AUDIO_CAPTURE=out.wav records up to 60 s of the device output, written on exit.
    std::vector<float> capture;
    std::filesystem::path capture_path;

    // Last member: destroyed first, so the device thread stops before anything it reads.
    std::unique_ptr<AudioSubsystem> subsystem;
    std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)> stream{nullptr,
                                                                               SDL_DestroyAudioStream};

    Impl(const std::filesystem::path &assets, bool requested, const std::filesystem::path &bank)
        : enabled(requested), directory(assets) {
        if (!enabled) {
            return;
        }
        // Speaking.mkf alone is 57 MB: opening it on the first voice mid-game stalled the main
        // thread for a long time. Read both PCM archives in the background right away.
        archive_loader = std::async(std::launch::async, [this] {
            auto effect_archive = assets::Archive(asset_path(directory, "effect.mkf"));
            auto voice_archive = assets::Archive(asset_path(directory, "speaking.mkf"));
            effects.emplace(std::move(effect_archive));
            voices.emplace(std::move(voice_archive));
        });
        if (const auto *path = std::getenv("RICH4_AUDIO_CAPTURE"); path && *path) {
            capture_path = path;
            capture.reserve(std::size_t(sample_rate) * channels * 60);
        }
        subsystem = std::make_unique<AudioSubsystem>();
        initialize_music(bank);
        stream.reset(SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &mixer_format, feed_callback,
                                               this));
        if (!stream) {
            throw sdl_error("open SDL audio stream");
        }
        if (!SDL_AddEventWatch(lifecycle_watch, this)) {
            throw sdl_error("watch application lifecycle for audio");
        }
        if (!SDL_ResumeAudioStreamDevice(stream.get())) {
            throw sdl_error("resume SDL audio stream");
        }
    }

    ~Impl() {
        // Stop the only hardware-facing object before releasing the software mixer and its
        // banks. No FluidSynth audio driver is ever constructed.
        if (stream) {
            SDL_RemoveEventWatch(lifecycle_watch, this);
            stream.reset(); // after this the device thread no longer calls feed()
        }
        write_capture();
        if (archive_loader.valid())
            archive_loader.wait();
        player.reset();
        ogg.close();
        synth.reset();
        settings.reset();
    }

    // Android/iOS: stop pulling samples while backgrounded and continue exactly where the music
    // was on return (MIDI time follows rendered samples). Desktop never sends these events.
    static bool SDLCALL lifecycle_watch(void *self, SDL_Event *event) {
        auto &impl = *static_cast<Impl *>(self);
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND)
            SDL_PauseAudioStreamDevice(impl.stream.get());
        else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
            SDL_ResumeAudioStreamDevice(impl.stream.get());
        return true;
    }

    // The Steam release has no MIDIs, only track02.ogg ... track26.ogg. Use those when no playlist
    // MIDI exists. Returns whether Ogg music was set up.
    bool initialize_ogg_music() {
#ifndef RICH4_HAVE_VORBIS
        return false;
#else
        const auto exists = [this](std::string_view name) {
            try {
                asset_path(directory, name);
                return true;
            } catch (const assets::AssetError &) {
                return false;
            }
        };
        if (std::any_of(music_files.begin(), music_files.end(), exists))
            return false;
        const auto add = [&](std::string_view midi) {
            const auto name = ogg_name(midi);
            if (exists(name))
                ogg_paths.emplace(midi, asset_path(directory, name));
        };
        std::for_each(music_files.begin(), music_files.end(), add);
        std::for_each(context_files.begin() + 1, context_files.end(), add); // skip "silence"
        if (ogg_paths.empty())
            return false;
        if (ogg_paths.size() < music_files.size() + context_files.size() - 1)
            diagnostic = "some Ogg music tracks are missing; those screens stay silent";
        if (const auto error = start_track_locked(); !error.empty()) // no device yet: no lock needed
            throw std::runtime_error(error);
        return true;
#endif
    }

    void initialize_music(std::filesystem::path bank) {
        if (initialize_ogg_music())
            return;
        if (bank.empty()) {
            if (const auto *environment = std::getenv("RICH4_SOUNDFONT")) {
                bank = environment;
            }
        }
#ifdef __APPLE__
        if (bank.empty()) {
            bank = "/System/Library/Components/CoreAudio.component/Contents/Resources/"
                   "gs_instruments.dls";
        }
#endif
        if (bank.empty() || !std::filesystem::is_regular_file(bank)) {
            diagnostic = "MIDI soundbank unavailable; set RICH4_SOUNDFONT to a General MIDI SF2/DLS file";
            return;
        }
        // All MIDIs are tiny (~0.5 MB together): read them now so neither a context push nor the
        // playlist advancing on the audio thread ever touches the file system. A missing MIDI only
        // keeps that music silent (the Steam release ships Ogg tracks instead of MIDIs).
        for (const auto file : music_files) {
            try {
                midi_cache.emplace(file, read_bytes(asset_path(directory, file)));
            } catch (const std::exception &error) {
                diagnostic = error.what();
            }
        }
        for (const auto file : context_files) {
            if (file == "silence")
                continue;
            try {
                midi_cache.emplace(file, read_bytes(asset_path(directory, file)));
            } catch (const std::exception &error) {
                diagnostic = error.what();
            }
        }
        // The original MIDIs carry meta events FluidSynth only warns about; keep stderr for errors.
        fluid_set_log_function(FLUID_WARN, nullptr, nullptr);
        fluid_set_log_function(FLUID_INFO, nullptr, nullptr);
        settings.reset(new_fluid_settings());
        if (!settings) {
            throw std::runtime_error("cannot create FluidSynth settings");
        }
        fluid_settings_setnum(settings.get(), "synth.sample-rate", sample_rate);
        fluid_settings_setnum(settings.get(), "synth.gain", music_gain);
        fluid_settings_setstr(settings.get(), "player.timing-source", "sample");
        fluid_settings_setint(settings.get(), "synth.reverb.active", 0);
        fluid_settings_setint(settings.get(), "synth.chorus.active", 0);
        synth.reset(new_fluid_synth(settings.get()));
        if (!synth || fluid_synth_sfload(synth.get(), bank.string().c_str(), 1) < 0) {
            synth.reset();
            diagnostic = "FluidSynth could not load MIDI soundbank: " + bank.string();
            return;
        }
        if (const auto error = start_track_locked(); !error.empty()) // no device yet: no lock needed
            throw std::runtime_error(error);
    }

    void log(std::string_view what, std::string_view detail) const {
        if (trace)
            trace(what, detail);
    }
    std::string_view current_file_locked() const {
        return context ? music_file(*context) : music_files[track - 1];
    }
    std::string current_file() const {
        const std::lock_guard lock(mutex);
        return std::string(current_file_locked());
    }
    // (Re)starts whatever `context`/`track` select, from `tick`. With music switched off the
    // start is deferred (player_stale) so a muted game never renders MIDI. Cheap (no IO): the
    // MIDI bytes were read at startup. Returns an error message instead of throwing because the
    // audio thread calls it when the playlist advances. Caller holds `mutex`.
    std::string start_track_locked(int tick = 0) {
        if (!synth && ogg_paths.empty()) {
            return {};
        }
        player.reset();
        ogg.close();
        if (synth)
            fluid_synth_system_reset(synth.get());
        if (!music_on) {
            player_stale = true;
            resume_tick = tick;
            return {};
        }
        player_stale = false;
        if (context == Music::silence) {
            return {}; // no player: silence until popped
        }
        const auto file = current_file_locked();
        if (!ogg_paths.empty()) { // `tick` is a PCM frame for Ogg music
            const auto found = ogg_paths.find(file);
            return found == ogg_paths.end() ? std::string() : ogg.open(found->second, tick);
        }
        const auto cached = midi_cache.find(file);
        if (cached == midi_cache.end()) {
            return {}; // missing context MIDI (reported at startup): silent, game keeps running
        }
        const auto &bytes = cached->second;
        player.reset(new_fluid_player(synth.get()));
        if (!player || fluid_player_add_mem(player.get(), bytes.data(), bytes.size()) != FLUID_OK) {
            player.reset();
            return "cannot load original MIDI " + std::string(file);
        }
        if (context) {
            fluid_player_set_loop(player.get(), -1); // context MIDIs loop until popped (fcn_00454d2c)
        }
        if (fluid_player_play(player.get()) != FLUID_OK) {
            player.reset();
            return "cannot start original MIDI " + std::string(file);
        }
        if (tick > 0) {
            fluid_player_seek(player.get(), tick); // resume where push_music left it
        }
        return {};
    }
    void start_track(int tick = 0) { // main thread
        std::string error;
        {
            const std::lock_guard lock(mutex);
            error = start_track_locked(tick);
        }
        if (!error.empty())
            throw std::runtime_error(error);
    }
    int current_tick_locked() {
        if (player_stale)
            return resume_tick;
        if (ogg.is_open())
            return int(ogg.position());
        return player ? std::max(0, fluid_player_get_current_tick(player.get())) : 0;
    }
    void push(Music music, bool save) {
        log(save ? "music_push" : "music_replace", music_file(music));
        std::string error;
        {
            const std::lock_guard lock(mutex);
            if (save || music_stack.empty())
                music_stack.push_back({context, track, current_tick_locked()});
            context = music;
            error = start_track_locked();
        }
        if (!error.empty())
            throw std::runtime_error(error);
    }
    void pop() {
        if (music_stack.empty())
            return;
        const auto saved = music_stack.back();
        music_stack.pop_back();
        std::string file, error;
        {
            const std::lock_guard lock(mutex);
            context = saved.context;
            track = saved.track;
            file = current_file_locked();
        }
        log("music_pop", file);
        start_track(saved.tick);
    }

    // Main thread only; may block on the startup archive loader (normally long finished).
    const std::vector<float> &load_clip(bool voice, std::size_t index) {
        const auto key = std::pair{voice, index};
        if (const auto found = samples.find(key); found != samples.end()) {
            return found->second;
        }
        if (archive_loader.valid()) {
            try {
                archive_loader.get();
            } catch (const std::exception &error) {
                archive_error = error.what();
            }
        }
        auto &archive = voice ? voices : effects;
        if (!archive) // a missing archive fails at first use, as before
            throw assets::AssetError(archive_error.empty() ? "audio archive unavailable" : archive_error);
        auto pcm = decode_wave(archive->decode(index));
        return samples.emplace(key, std::move(pcm)).first->second;
    }

    // Caller holds `mutex`.
    void start_clip_locked(const std::vector<float> &pcm, bool voice, bool keep_queue, bool loop = false,
                           Loop channel = Loop::spinner) {
        if (voice) {
            if (!keep_queue)
                speech_queue.clear();
            std::erase_if(clips, [](const Clip &clip) { return clip.voice; });
            ++stats.voices_started;
        } else {
            ++stats.effects_started;
        }
        // Bound bursts of user input. Voice always has its own single slot;
        // effects can overlap, with the oldest effect replaced after 16 voices.
        if (!voice &&
            std::count_if(clips.begin(), clips.end(), [](const Clip &clip) { return !clip.voice; }) >= 16) {
            clips.erase(
                std::find_if(clips.begin(), clips.end(), [](const Clip &clip) { return !clip.voice; }));
        }
        clips.push_back({&pcm, 0, voice, loop, channel});
    }

    void play_clip(bool voice, std::size_t index, bool keep_queue = false, bool loop = false,
                   Loop channel = Loop::spinner) {
        if (!enabled) {
            return;
        }
        const auto &pcm = load_clip(voice, index); // decode outside the lock
        const std::lock_guard lock(mutex);
        start_clip_locked(pcm, voice, keep_queue, loop, channel);
    }

    // ---- audio thread ----
    static void SDLCALL feed_callback(void *self, SDL_AudioStream *stream, int additional_amount, int) {
        static_cast<Impl *>(self)->feed(stream, additional_amount);
    }
    void feed(SDL_AudioStream *output, int additional_amount) noexcept {
        if (additional_amount <= 0)
            return;
        const auto begin = SDL_GetTicksNS();
        std::array<float, block_frames * channels> block;
        const std::lock_guard lock(mutex);
        // Whole blocks keep MIDI timing identical to the old main-thread mixer; the (< 1 block)
        // surplus stays queued in the stream and shortens the next request.
        int produced = 0;
        while (produced < additional_amount) {
            render_block_locked(block);
            const int byte_count = int(block.size() * sizeof(float));
            if (!SDL_PutAudioStreamData(output, block.data(), byte_count)) {
                render_error = std::string("queue native mixed audio: ") + SDL_GetError();
                break;
            }
            capture_block_locked(block);
            produced += byte_count;
        }
        // Real-time budget: the device can only starve if mixing ever runs slower than playback.
        const auto end = SDL_GetTicksNS();
        const auto spent_ns = end - begin;
        const auto audio_ns = std::uint64_t(produced / bytes_per_frame) * 1000000000ULL / sample_rate;
        stats.max_render_us = std::max<std::uint64_t>(stats.max_render_us, spent_ns / 1000);
        if (spent_ns > audio_ns) {
            ++stats.underruns;
            stats.starved_ms += (spent_ns - audio_ns) / 1000000;
        }
    }

    void next_track_locked() {
        track = track % 8 + 1;
        pending_log.emplace_back("music_next", current_file_locked());
        if (auto error = start_track_locked(); !error.empty())
            render_error = std::move(error);
    }

    void render_block_locked(std::span<float> output) noexcept {
        std::fill(output.begin(), output.end(), 0.0F);
        if (music_on && ogg.is_open()) {
            // Context music loops until popped; a finished playlist track advances.
            if (!ogg.mix(output, ogg_gain, context.has_value()) && !context)
                next_track_locked();
            stats.music_frames += block_frames;
        }
        if (music_on && player) {
            if (fluid_player_get_status(player.get()) == FLUID_PLAYER_DONE && !context)
                next_track_locked();
            if (player && fluid_synth_write_float(synth.get(), block_frames, output.data(), 0, 2, output.data(), 1,
                                                  2) != FLUID_OK) {
                render_error = "MIDI software rendering failed";
                player.reset(); // keep effects and voices going without music
            }
            if (player)
                stats.music_frames += block_frames;
        }
        for (auto &clip : clips) {
            std::size_t written = 0;
            do {
                const auto available = clip.samples->size() - clip.position;
                const auto count = std::min(output.size() - written, available);
                for (std::size_t i = 0; i < count; ++i) {
                    output[written + i] += (*clip.samples)[clip.position + i] * clip_gain;
                }
                clip.position += count;
                written += count;
                if (clip.position == clip.samples->size()) {
                    ++stats.clips_completed;
                    if (clip.loop && !clip.samples->empty()) {
                        clip.position = 0;
                    }
                }
            } while (clip.loop && !clip.samples->empty() && written < output.size());
        }
        std::erase_if(clips, [](const Clip &clip) { return clip.position == clip.samples->size(); });
        if (!speech_queue.empty() &&
            std::none_of(clips.begin(), clips.end(), [](const Clip &clip) { return clip.voice; })) {
            const auto next = speech_queue.front();
            speech_queue.erase(speech_queue.begin());
            pending_log.emplace_back("speech", std::to_string(next.index));
            start_clip_locked(*next.samples, true, true);
        }
        for (const auto sample : output) {
            const float magnitude = std::abs(sample);
            stats.peak = std::max(stats.peak, magnitude);
            if (magnitude > 1.0F)
                ++stats.clipped_samples; // what the old hard clamp would have squared off
        }
        stats.limited_samples += limiter.process(output);
        for (const auto sample : output) {
            if (std::abs(sample) > 0.000001F) {
                ++stats.nonzero_samples;
            }
        }
        stats.rendered_frames += block_frames;
    }

    void capture_block_locked(std::span<const float> block) {
        if (capture_path.empty())
            return;
        const auto room = capture.capacity() - capture.size(); // never reallocates on this thread
        capture.insert(capture.end(), block.begin(), block.begin() + std::ptrdiff_t(std::min(room, block.size())));
    }
    void write_capture() const {
        if (capture_path.empty())
            return;
        std::ofstream out(capture_path, std::ios::binary);
        const auto put32 = [&out](std::uint32_t value) { out.write(reinterpret_cast<const char *>(&value), 4); };
        const auto put16 = [&out](std::uint16_t value) { out.write(reinterpret_cast<const char *>(&value), 2); };
        const auto data_bytes = std::uint32_t(capture.size() * 2);
        out.write("RIFF", 4);
        put32(36 + data_bytes);
        out.write("WAVEfmt ", 8);
        put32(16);
        put16(1); // PCM
        put16(channels);
        put32(sample_rate);
        put32(sample_rate * channels * 2);
        put16(channels * 2);
        put16(16);
        out.write("data", 4);
        put32(data_bytes);
        for (const float sample : capture) {
            const auto value = std::int16_t(std::lround(std::clamp(sample, -1.0F, 1.0F) * 32767.0F));
            put16(std::uint16_t(value));
        }
    }

    // ---- main thread, once per frame ----
    void update() {
        if (!enabled) {
            return;
        }
        const auto now_ns = SDL_GetTicksNS();
        std::vector<std::pair<std::string_view, std::string>> lines;
        std::string error;
        {
            const std::lock_guard lock(mutex);
            if (last_update_ns)
                stats.max_update_gap_ms =
                    std::max<std::uint64_t>(stats.max_update_gap_ms, (now_ns - last_update_ns) / 1000000);
            lines.swap(pending_log);
            error.swap(render_error);
        }
        last_update_ns = now_ns;
        for (const auto &[what, detail] : lines)
            log(what, detail);
        if (!error.empty() && !error_reported) {
            error_reported = true;
            diagnostic = error;
            std::fprintf(stderr, "audio: %s\n", error.c_str());
        }
    }
};

Audio::Audio(const std::filesystem::path &directory, bool enabled, const std::filesystem::path &soundbank)
    : impl_(std::make_unique<Impl>(directory, enabled, soundbank)) {}
Audio::~Audio() = default;
Audio::Audio(Audio &&) noexcept = default;
Audio &Audio::operator=(Audio &&) noexcept = default;

void Audio::play_sound(std::size_t index) {
    impl_->log("sound", std::to_string(index));
    impl_->play_clip(false, index);
}

void Audio::play_loop(std::size_t index, Loop channel) {
    auto &slot = impl_->loops.at(std::size_t(channel));
    if (slot == index) {
        return; // already looping here: keep it seamless
    }
    stop_loop(channel);
    slot = index;
    impl_->log("loop_start", std::string(loop_names[std::size_t(channel)]) + " " + std::to_string(index));
    impl_->play_clip(false, index, false, true, channel);
}

void Audio::stop_loop(Loop channel) {
    auto &slot = impl_->loops.at(std::size_t(channel));
    if (!slot) {
        return;
    }
    impl_->log("loop_stop", std::string(loop_names[std::size_t(channel)]) + " " + std::to_string(*slot));
    slot.reset();
    const std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->clips,
                  [channel](const Impl::Clip &clip) { return clip.loop && clip.channel == channel; });
}

void Audio::stop_all_loops() {
    for (std::size_t channel = 0; channel < impl_->loops.size(); ++channel)
        stop_loop(Loop(channel));
}

std::optional<std::size_t> Audio::looping(Loop channel) const noexcept {
    return std::size_t(channel) < impl_->loops.size() ? impl_->loops[std::size_t(channel)] : std::nullopt;
}

void Audio::play_voice(std::uint8_t character, std::uint16_t category) {
    if (character >= 12 || category >= 27) {
        throw std::invalid_argument("voice character/category outside original event table");
    }
    impl_->play_clip(true, 1050U + character * 27U + category);
}

void Audio::queue_speech(std::size_t index) {
    if (!impl_->enabled) {
        return;
    }
    const auto &pcm = impl_->load_clip(true, index); // decode now: the audio thread starts it later
    bool now = false;
    {
        const std::lock_guard lock(impl_->mutex);
        now = impl_->speech_queue.empty() &&
              std::none_of(impl_->clips.begin(), impl_->clips.end(), [](const Impl::Clip &clip) { return clip.voice; });
        if (now)
            impl_->start_clip_locked(pcm, true, true);
        else
            impl_->speech_queue.push_back({index, &pcm});
    }
    if (now)
        impl_->log("speech", std::to_string(index));
}

std::uint32_t Audio::play_speech(std::size_t index) {
    if (!impl_->enabled) {
        return 0;
    }
    impl_->log("speech", std::to_string(index));
    const auto &pcm = impl_->load_clip(true, index);
    impl_->play_clip(true, index);
    return static_cast<std::uint32_t>(pcm.size() / channels * 1000 / sample_rate);
}

void Audio::stop_voice() {
    const std::lock_guard lock(impl_->mutex);
    impl_->speech_queue.clear();
    std::erase_if(impl_->clips, [](const Impl::Clip &clip) { return clip.voice; });
}

bool Audio::voice_active() const noexcept {
    const std::lock_guard lock(impl_->mutex);
    return std::any_of(impl_->clips.begin(), impl_->clips.end(), [](const Impl::Clip &clip) { return clip.voice; });
}

void Audio::set_music(bool enabled) {
    std::string error;
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->music_on == enabled)
            return;
        impl_->music_on = enabled;
        if (enabled && impl_->player_stale)
            error = impl_->start_track_locked(impl_->resume_tick); // resume what changed while muted
    }
    impl_->log("music_enabled", enabled ? "on" : "off");
    if (!error.empty())
        throw std::runtime_error(error);
}

void Audio::select_music(unsigned track) {
    if (track > 8) {
        throw std::invalid_argument("normal MIDI playlist selection must be 0 through 8");
    }
    std::string file;
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->track = track == 0 ? impl_->track % 8 + 1 : track;
        impl_->context.reset();
        file = impl_->current_file_locked();
    }
    impl_->music_stack.clear();
    impl_->log("music_select", file);
    impl_->start_track();
}

void Audio::push_music(Music music) {
    impl_->push(music, true);
}
void Audio::pop_music() {
    impl_->pop();
}
void Audio::replace_music(Music music) {
    impl_->push(music, false);
}
std::size_t Audio::music_depth() const noexcept {
    return impl_->music_stack.size();
}
std::optional<Music> Audio::music_context() const noexcept {
    return impl_->context; // written only by the main thread
}
std::string Audio::music_name() const {
    return impl_->current_file();
}
void Audio::set_trace(Trace trace) {
    impl_->trace = std::move(trace);
}

void Audio::update() {
    impl_->update();
}
bool Audio::enabled() const noexcept {
    return impl_->enabled;
}
bool Audio::music_available() const noexcept {
    return impl_->synth != nullptr || !impl_->ogg_paths.empty(); // set once at construction
}
bool Audio::music_enabled() const noexcept {
    return impl_->music_on; // written only by the main thread
}
unsigned Audio::music_track() const noexcept {
    const std::lock_guard lock(impl_->mutex); // the audio thread advances the playlist
    return impl_->track;
}
Statistics Audio::statistics() const noexcept {
    const std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}
int Audio::queued_bytes() const {
    return impl_->stream ? SDL_GetAudioStreamQueued(impl_->stream.get()) : 0;
}
const std::string &Audio::music_diagnostic() const noexcept {
    return impl_->diagnostic;
}

} // namespace rich4::audio
