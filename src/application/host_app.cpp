// Super Mario Sunshine executable: SDL window and OpenGL ES context, Starlet's renderer on
// the GPU thread, controller sampling on this thread, and the game's main() on
// the guest CPU.
#include "application/host_app.hpp"
#include "platform/memory.hpp"
#include "platform/sdk_layout.hpp"

#include "starlet/ai/ai.hpp"
#include "starlet/card/card.hpp"
#include "starlet/card/sdk.hpp"
#include "starlet/dvd/dvd.hpp"
#include "starlet/gx/gles/renderer.hpp"
#include "starlet/gx_fifo/frame_trace.hpp"
#include "starlet/gx_fifo/gles_renderer.hpp"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/video.hpp"
#include "starlet/host/controllers.hpp"
#include "starlet/host/quit.hpp"
#include "starlet/os/crash_report.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/os/profiler.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/thread_name.hpp"
#include "starlet/pad/pad.hpp"

#include <GLES3/gl31.h>
#include <SDL.h>
#ifdef PORT_HAS_OVERLAY
#include <spo.h>
#endif

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <link.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern const char sunshine_port_version[];
extern "C" void sunshine_gx_flush_dirty_state(void);

namespace {

// A fatal signal logs where it happened (starlet::os crash report), with the executable's build
// ID, so a player's log.txt names the failure and maps onto the unstripped build. Installed before
// main, which prints the build ID.
__attribute__((constructor(101))) void report_fatal_signals() { starlet::os::install_fatal_signal_report("sunshine"); }

struct Display {
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    Display() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        // Created at the desktop's size: without a window manager (an X11 session with none)
        // a fullscreen-desktop window keeps its creation size, and 0x0 becomes 1x1.
        SDL_DisplayMode desktop{};
        if (SDL_GetDesktopDisplayMode(0, &desktop) != 0) desktop.w = desktop.h = 0;
        window = SDL_CreateWindow("Super Mario Sunshine", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, desktop.w, desktop.h,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (window) gl = SDL_GL_CreateContext(window);
        if (!gl) throw std::runtime_error(SDL_GetError());
        // SDL's default of 0 flips mid-scanout on KMS, so a frame shows torn across a line.
        if (SDL_GL_SetSwapInterval(1) != 0) std::printf("Display: no vsync (%s)\n", SDL_GetError());
    }
};

std::atomic<int> drawable_width{0}, drawable_height{0};

// The settings menu and performance overlay (sbc-port-overlay). Its calls are serialized on one
// mutex: events and the controller sample run on the main thread, frames on the GL thread. Every
// function compiles to nothing without PORT_HAS_OVERLAY.
namespace overlay {
#ifdef PORT_HAS_OVERLAY
std::mutex lock;
bool active = false;

void* resolve(const char* name, void*) { return SDL_GL_GetProcAddress(name); }

// The GL context current on the calling thread.
void init() {
    mkdir("conf", 0755);
    spo_config config{};
    config.struct_size = sizeof config;
    config.abi_version = SPO_ABI_VERSION;
    config.get_proc_address = resolve;
    config.config_path = "conf/overlay.ini";
    config.port_name = "Super Mario Sunshine";
    config.port_version = sunshine_port_version;
    const spo_result result = spo_init(&config);
    active = result == SPO_OK;
    if (!active) std::printf("Overlay: unavailable (%d)\n", int(result));
}
void handle(const SDL_Event& event) {
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    spo_handle_sdl_event(reinterpret_cast<const union SDL_Event*>(&event));
}
bool captured() {
    if (!active) return false;
    std::lock_guard<std::mutex> guard(lock);
    return spo_input_captured() != 0;
}
// Before every swap, on the thread that owns the context.
void frame(int width, int height) {
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    spo_frame(std::uint32_t(width), std::uint32_t(height));
}
void save() {
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    if (spo_config_dirty()) spo_config_save();
}
// A safe point: the menu is closed and drained. A failed save is not retried until the next change.
void save_when_closed() {
    static bool failed = false;
    if (!active || failed) return;
    std::lock_guard<std::mutex> guard(lock);
    if (spo_input_captured() || !spo_config_dirty()) return;
    if (spo_config_save() != SPO_OK) {
        failed = true;
        std::printf("Overlay: settings not saved (%d)\n", int(spo_last_error()));
    }
}
// On the GL thread, before it releases the context.
void shutdown() {
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    spo_shutdown();
    active = false;
}
#else
void init() {}
void handle(const SDL_Event&) {}
bool captured() { return false; }
void frame(int, int) {}
void save() {}
void save_when_closed() {}
void shutdown() {}
#endif
}

// Development captures: SMS_CAPTURE_FRAMES lists swap numbers whose presented
// image is written as SMS_CAPTURE_DIR/sunshine-frame-N.ppm.
class Captures {
public:
    Captures() {
        const char* frames = std::getenv("SMS_CAPTURE_FRAMES");
        const char* dir = std::getenv("SMS_CAPTURE_DIR");
        directory_ = dir ? dir : "/tmp";
        for (const char* p = frames; p && *p;) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(p, &end, 10);
            if (end == p) break;
            if (count_ < sizeof frames_ / sizeof frames_[0]) frames_[count_++] = n;
            p = *end ? end + 1 : end;
        }
    }
    // Submission thread, context current, before the swap.
    void before_swap() {
        ++swaps_;
        for (unsigned i = 0; i < count_; ++i)
            if (frames_[i] == swaps_) write(swaps_);
        if (const unsigned long shot = shot_.exchange(0)) write(shot, "sunshine-shot-");
    }
    // Any thread: the next presented image is written as sunshine-shot-ID.ppm.
    void shoot(unsigned long id) { shot_.store(id); }

private:
    void write(unsigned long frame, const char* prefix = "sunshine-frame-") {
        const int w = drawable_width, h = drawable_height;
        std::string pixels(std::size_t(w) * h * 4, '\0');
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &pixels[0]);
        const std::string path = directory_ + "/" + prefix + std::to_string(frame) + ".ppm";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; --y)
                for (int x = 0; x < w; ++x) std::fwrite(&pixels[(std::size_t(y) * w + x) * 4], 1, 3, f);
            std::fclose(f);
            std::printf("capture: frame %lu -> %s\n", frame, path.c_str());
        }
    }
    std::string directory_;
    std::atomic<unsigned long> shot_{0};
    unsigned long frames_[16] = {};
    unsigned count_ = 0;
    unsigned long swaps_ = 0;
};

Captures& shared_captures() {
    static Captures captures;
    return captures;
}

// GameCube controller from the handheld's pad. Face buttons follow the printed
// Nintendo layout (A right, B below, X above, Y left); SDL names them by the
// Xbox positions, so label A reads SDL B, label B SDL A, label X SDL Y, label Y SDL X.
extern "C" unsigned sunshine_game_state();
extern "C" void sunshine_request_warp(int code);
extern "C" void sunshine_pad_read_hook();

class Controller {
public:
    // Development: SMS_PRESS="seconds:INPUT[:milliseconds],..." holds each input at
    // that time since start, for 150 ms unless given. Inputs: A B X Y Z L R START
    // UP DOWN LEFT RIGHT (D-pad), STICK_UP/DOWN/LEFT/RIGHT and C_UP/DOWN/LEFT/RIGHT
    // (full deflection); L and R also press their analog triggers fully. SHOT writes the next
    // presented image as SMS_CAPTURE_DIR/sunshine-shot-<stage><milliseconds>.ppm. "seconds:INPUT[:milliseconds]@STATE[:STAGE[:PROGRESS]]" presses only while the
    // game is in that TApplication state, stage and title/file-select TCardLoad progress. SMS_PRESS_UNTIL="STATE[:STAGE]>..." ends the script once the game has
    // passed through those TApplication states (9 file select, 5 gameplay) and stages in order,
    // and a final "+SECONDS" later, keeping only the presses gated on a stage; state changes are
    // logged as "game state:" lines. A time written "+seconds" counts from the latest entry into the
    // press's gated state and stage (or another scenario of it) instead of from start.
    Controller() {
        for (const char* p = std::getenv("SMS_PRESS_UNTIL"); p && *p;) {
            char* end = nullptr;
            Until step{int(std::strtol(p, &end, 0)), -1};
            if (end == p) break;
            if (*end == ':') step.stage = int(std::strtol(end + 1, &end, 0));
            until_.push_back(step);
            if (*end == '+') tail_ms_ = Uint32(std::strtod(end + 1, &end) * 1000);
            p = *end == '>' ? end + 1 : end;
        }
        start_ = SDL_GetTicks();
        const char* script = std::getenv("SMS_PRESS");
        for (const char* p = script; p && *p;) {
            char* end = nullptr;
            const bool relative = *p == '+';
            const double seconds = std::strtod(p + relative, &end);
            if (end == p + relative || *end != ':') break;
            p = end + 1;
            const char* name_end = p;
            while (*name_end && *name_end != ',' && *name_end != ':' && *name_end != '@') ++name_end;
            const std::string name(p, name_end);
            Uint32 hold = 150;
            if (*name_end == ':') {
                hold = Uint32(std::strtoul(name_end + 1, &end, 10));
                name_end = end;
            }
            int only_state = -1, only_stage = -1, only_progress = -1;
            if (*name_end == '@') {
                only_state = int(std::strtol(name_end + 1, &end, 0));
                if (*end == ':') only_stage = int(std::strtol(end + 1, &end, 0));
                if (*end == ':') only_progress = int(std::strtol(end + 1, &end, 0));
                name_end = end;
            }
            static const struct { const char* name; std::uint16_t bits; std::int8_t sx, sy, cx, cy; } names[] = {
                {"A", 0x0100}, {"B", 0x0200}, {"X", 0x0400}, {"Y", 0x0800}, {"START", 0x1000}, {"Z", 0x0010},
                {"R", 0x0020}, {"L", 0x0040}, {"UP", 0x0008}, {"DOWN", 0x0004}, {"LEFT", 0x0001}, {"RIGHT", 0x0002},
                {"STICK_UP", 0, 0, 72}, {"STICK_DOWN", 0, 0, -72}, {"STICK_LEFT", 0, -72, 0}, {"STICK_RIGHT", 0, 72, 0},
                {"C_UP", 0, 0, 0, 0, 59}, {"C_DOWN", 0, 0, 0, 0, -59}, {"C_LEFT", 0, 0, 0, -59, 0}, {"C_RIGHT", 0, 0, 0, 59, 0}};
            for (const auto& n : names)
                if (name == n.name)
                    script_.push_back({Uint32(seconds * 1000), hold, n.bits, n.sx, n.sy, n.cx, n.cy, only_state, only_stage,
                                       only_progress, relative && only_state >= 0});
            if (name == "SHOT")
                script_.push_back({Uint32(seconds * 1000), 0, 0, 0, 0, 0, 0, only_state, only_stage, only_progress,
                                   relative && only_state >= 0, true, 0});
            while (*name_end && *name_end != ',') ++name_end;
            p = *name_end ? name_end + 1 : name_end;
        }
        // SMS_WARP=CODE changes stage when the press script ends, through TMarDirector::setNextStage
        // on the game thread (CODE: (stage + 1) << 8 | scenario).
        // SMS_WARP=CODE,CODE,... warps to each next code SMS_WARP_EVERY seconds (default 60) after
        // arriving at the previous one, or 150 s after asking for it.
        if (const char* warp = std::getenv("SMS_WARP")) {
            for (const char* p = warp; *p;) {
                char* end = nullptr;
                const int code = int(std::strtol(p, &end, 0));
                if (end == p) break;
                warps_.push_back(code);
                p = *end == ',' ? end + 1 : end;
            }
            warp_ = warps_.empty() ? 0 : warps_.front();
            if (const char* every = std::getenv("SMS_WARP_EVERY")) warp_every_ms_ = Uint32(std::atof(every) * 1000);
            starlet::pad::set_read_hook(sunshine_pad_read_hook);
        }
        if (std::getenv("SMS_PAD_TRACE"))
            std::printf("controller: %zu scripted presses\n", script_.size());
        attach_virtual_pad();
    }
    void sample() {
        pads_.update();
        drive_virtual_pad();
        starlet::pad::PadState state;
        // While the menu has the controller the game reads a pad at rest, still connected.
        auto raw = pads_.state(0);
        if (raw.connected && overlay::captured()) raw = starlet::host::Gamepad{true};
        const auto pad = starlet::host::to_gamecube(raw, mapping());
        if (pad.connected) {
            state.connected = true;
            state.buttons = pad.buttons;
            state.stick_x = pad.stick_x;
            state.stick_y = pad.stick_y;
            state.substick_x = pad.substick_x;
            state.substick_y = pad.substick_y;
            state.trigger_left = pad.trigger_left;
            state.trigger_right = pad.trigger_right;
        }
        const Uint32 now = SDL_GetTicks() - start_;
        static const bool trace = std::getenv("SMS_PAD_TRACE") != nullptr;
        if (trace && now / 10000 != heartbeat_) {
            heartbeat_ = now / 10000;
            std::printf("controller: %u ms sampled\n", now);
            std::fflush(stdout);
        }
        if (watch_) {
            const unsigned game = sunshine_game_state();
            if (game != game_state_) {
                game_state_ = game;
                // Entering a state and stage, or another scenario of it, restarts its relative presses.
                if ((game & 0xffffff) != (last_area_ & 0xffffff)) {
                    const int key = int((game >> 8) & 0xffff);
                    const auto known = std::find_if(entered_.begin(), entered_.end(), [&](const auto& e) { return e.first == key; });
                    if (known == entered_.end()) entered_.push_back({key, now});
                    else known->second = now;
                }
                last_area_ = game;
                std::printf("game state: %u ms app %u stage 0x%02x scenario %u card progress %d\n", now,
                            (game >> 16) & 0xff, (game >> 8) & 0xff, game & 0xff, int(game >> 24) - 1);
                std::fflush(stdout);
                if (matched_ < until_.size() && int((game >> 16) & 0xff) == until_[matched_].state &&
                    (until_[matched_].stage < 0 || int((game >> 8) & 0xff) == until_[matched_].stage) &&
                    ++matched_ == until_.size())
                    end_at_ = now + tail_ms_;
            }
            if (end_at_ && now >= end_at_ && !ended_) {
                ended_ = true;
                std::printf("controller: press script ended at %u ms\n", now);
                script_.erase(std::remove_if(script_.begin(), script_.end(), [](const Press& press) { return press.stage < 0; }),
                              script_.end());
                if (warp_) {
                    std::printf("controller: warp to 0x%x\n", unsigned(warp_));
                    sunshine_request_warp(warp_);
                    warp_asked_ = now;
                }
            }
            if (ended_ && warp_index_ + 1 < warps_.size()) {
                const int code = warps_[warp_index_];
                const int stage = code >= 0x100 ? (code >> 8) - 1 : code;
                // Arrival: gameplay in the code's stage, and its scenario when the code names one.
                if (!warp_arrived_ && int((game_state_ >> 16) & 0xff) == 5 && int((game_state_ >> 8) & 0xff) == stage &&
                    (code < 0x100 || int(game_state_ & 0xff) == (code & 0xff)))
                    warp_arrived_ = now;
                if ((warp_arrived_ && now - warp_arrived_ >= warp_every_ms_) || now - warp_asked_ >= 150000) {
                    warp_ = warps_[++warp_index_];
                    warp_arrived_ = 0;
                    warp_asked_ = now;
                    std::printf("controller: warp to 0x%x\n", unsigned(warp_));
                    std::fflush(stdout);
                    sunshine_request_warp(warp_);
                }
            }
        }
        for (Press& press : script_) {
            Uint32 at = press.at;
            if (press.relative) {
                // Counted from the latest entry into the press's state and stage; until then it waits.
                const auto entered = std::find_if(entered_.begin(), entered_.end(), [&](const auto& e) {
                    return (e.first >> 8) == press.state && (press.stage < 0 || (e.first & 0xff) == press.stage);
                });
                if (entered == entered_.end()) continue;
                at += entered->second;
            }
            if (press.shot) {
                if (now >= at && press.fired != at && (press.state < 0 || int((game_state_ >> 16) & 0xff) == press.state) &&
                    (press.stage < 0 || int((game_state_ >> 8) & 0xff) == press.stage)) {
                    press.fired = at;
                    shared_captures().shoot((((game_state_ >> 8) & 0xff) * 100 + (game_state_ & 0xff)) * 1000000ul + press.at);
                }
                continue;
            }
            if (now >= at && now < at + press.hold &&
                (press.state < 0 || int((game_state_ >> 16) & 0xff) == press.state) &&
                (press.stage < 0 || int((game_state_ >> 8) & 0xff) == press.stage) &&
                (press.progress < 0 || int(game_state_ >> 24) - 1 == press.progress)) {
                state.connected = true;
                state.buttons |= press.buttons;
                if (press.buttons & 0x0040) state.trigger_left = 0xff;
                if (press.buttons & 0x0020) state.trigger_right = 0xff;
                if (press.stick_x || press.stick_y) {
                    state.stick_x = press.stick_x;
                    state.stick_y = press.stick_y;
                }
                if (press.c_x || press.c_y) {
                    state.substick_x = press.c_x;
                    state.substick_y = press.c_y;
                }
            }
        }
        starlet::pad::publish(0, state);
        for (unsigned channel = 1; channel < 4; ++channel) starlet::pad::publish(channel, {});
    }

private:
    // Development: SMS_VIRTUAL_PAD attaches an SDL virtual joystick laid out like the
    // handhelds' (16 buttons, 4 axes, 1 hat) of generic type, so SDL gives it no mapping of
    // its own: only an SDL_GAMECONTROLLERCONFIG line for its GUID (logged when missing)
    // makes it a controller, as on a device. SMS_VIRTUAL_PRESS="seconds:INPUT[:milliseconds],..."
    // holds its raw inputs, 150 ms unless given: bN (button N), hN.M (hat N at value M),
    // aN+ and aN- (axis N at full deflection).
    void attach_virtual_pad() {
        if (!std::getenv("SMS_VIRTUAL_PAD")) return;
        // SDL 2.0.14 API; the build's SDL headers are older, the devices' libraries newer.
        using Attach = int (*)(SDL_JoystickType, int, int, int);
        const auto attach = reinterpret_cast<Attach>(dlsym(RTLD_DEFAULT, "SDL_JoystickAttachVirtual"));
        set_button_ = reinterpret_cast<SetValue>(dlsym(RTLD_DEFAULT, "SDL_JoystickSetVirtualButton"));
        set_axis_ = reinterpret_cast<SetValue>(dlsym(RTLD_DEFAULT, "SDL_JoystickSetVirtualAxis"));
        set_hat_ = reinterpret_cast<SetValue>(dlsym(RTLD_DEFAULT, "SDL_JoystickSetVirtualHat"));
        if (!attach || !set_button_ || !set_axis_ || !set_hat_) {
            std::printf("Virtual pad: this SDL has no virtual joysticks\n");
            return;
        }
        const int index = attach(SDL_JOYSTICK_TYPE_UNKNOWN, 4, 16, 1);
        virtual_ = index >= 0 ? SDL_JoystickOpen(index) : nullptr;
        if (!virtual_) {
            std::printf("Virtual pad: %s\n", SDL_GetError());
            return;
        }
        const char* script = std::getenv("SMS_VIRTUAL_PRESS");
        for (const char* p = script; p && *p;) {
            char* end = nullptr;
            const double seconds = std::strtod(p, &end);
            if (end == p || *end != ':') break;
            p = end + 1;
            RawPress press{Uint32(seconds * 1000), 150, *p, 0, 0};
            if (*p) ++p;
            press.index = int(std::strtol(p, &end, 10));
            p = end;
            if (press.kind == 'h' && *p == '.') press.value = int(std::strtol(p + 1, &end, 10)), p = end;
            if (press.kind == 'a') press.value = *p == '-' ? -32768 : 32767, p += (*p == '+' || *p == '-');
            if (*p == ':') press.hold = Uint32(std::strtoul(p + 1, &end, 10)), p = end;
            if (press.kind == 'b' || press.kind == 'h' || press.kind == 'a') raw_.push_back(press);
            while (*p && *p != ',') ++p;
            if (*p) ++p;
        }
        std::printf("Virtual pad: %zu raw presses\n", raw_.size());
    }
    void drive_virtual_pad() {
        if (!virtual_) return;
        const Uint32 now = SDL_GetTicks() - start_;
        bool buttons[16] = {};
        Sint16 axes[4] = {};
        Uint8 hat = SDL_HAT_CENTERED;
        for (const RawPress& press : raw_) {
            if (now < press.at || now >= press.at + press.hold) continue;
            if (press.kind == 'b' && press.index < 16) buttons[press.index] = true;
            if (press.kind == 'a' && press.index < 4) axes[press.index] = Sint16(press.value);
            if (press.kind == 'h' && press.index == 0) hat = Uint8(press.value);
        }
        for (int i = 0; i < 16; ++i) set_button_(virtual_, i, buttons[i]);
        for (int i = 0; i < 4; ++i) set_axis_(virtual_, i, axes[i]);
        set_hat_(virtual_, 0, hat);
    }
    using SetValue = int (*)(SDL_Joystick*, int, int);
    struct RawPress {
        Uint32 at, hold;
        char kind;
        int index, value;
    };
    SDL_Joystick* virtual_ = nullptr;
    SetValue set_button_ = nullptr, set_axis_ = nullptr, set_hat_ = nullptr;
    std::vector<RawPress> raw_;
    // Face buttons by their printed Nintendo labels, shoulders as L and R, either trigger as
    // Z, sticks with a radial dead zone of 8000 to the console's 72 and 59.
    static const starlet::host::GameCubeMapping& mapping() {
        static const starlet::host::GameCubeMapping m = [] {
            using starlet::host::bit;
            using starlet::host::Input;
            using Shape = starlet::host::StickResponse::Shape;
            starlet::host::GameCubeMapping m;
            m.a = bit(Input::east);
            m.b = bit(Input::south);
            m.x = bit(Input::north);
            m.y = bit(Input::west);
            m.start = bit(Input::start);
            m.up = bit(Input::dpad_up);
            m.down = bit(Input::dpad_down);
            m.left = bit(Input::dpad_left);
            m.right = bit(Input::dpad_right);
            m.l = bit(Input::left_shoulder);
            m.r = bit(Input::right_shoulder);
            m.z = bit(Input::left_trigger) | bit(Input::right_trigger);
            m.trigger_threshold = 16000;
            m.trigger_left = {false, true};
            m.trigger_right = {false, true};
            m.main_stick = {Shape::radial, 8000, true, 32767.f, false, 72};
            m.c_stick = {Shape::radial, 8000, true, 32767.f, false, 59};
            return m;
        }();
        return m;
    }
    starlet::host::Controllers pads_{1, [](const char* line) {
                                         std::printf("%s\n", line);
                                         std::fflush(stdout);
                                     }};
    Uint32 heartbeat_ = 0;
    const bool watch_ = std::getenv("SMS_PRESS") != nullptr;
    unsigned game_state_ = ~0u;
    struct Until { int state, stage; };
    std::vector<Until> until_;
    size_t matched_ = 0;
    Uint32 tail_ms_ = 0, end_at_ = 0;
    int warp_ = 0;
    std::vector<int> warps_;
    size_t warp_index_ = 0;
    Uint32 warp_every_ms_ = 60000, warp_asked_ = 0, warp_arrived_ = 0;
    struct Press {
        Uint32 at, hold;
        std::uint16_t buttons;
        std::int8_t stick_x, stick_y, c_x, c_y;
        int state, stage, progress;
        bool relative;
        bool shot = false;
        Uint32 fired = 0;  // shots: the time it fired at, once per entry into its stage
    };
    std::vector<Press> script_;
    // (state << 8 | stage, ms since start) of each state and stage when first seen.
    std::vector<std::pair<int, Uint32>> entered_;
    unsigned last_area_ = ~0u;
    bool ended_ = false;
    Uint32 start_ = 0;
};

// Development: SMS_AUDIO_STATS prints, every 5 s, the DMA blocks finished, those
// the game had not refilled in time (waits) and replayed (repeats), and how often
// the device queue ran dry.
class AudioStats {
public:
    void sample() {
        if (!enabled_) return;
        const Uint32 now = SDL_GetTicks();
        if (now - last_ < 5000) return;
        last_ = now;
        const auto ai = starlet::ai::stats();
        std::printf("audio: %us blocks +%llu waits +%llu repeats +%llu waited %llu ms device empty +%llu\n", now / 1000,
                    (unsigned long long)(ai.completions - ai_.completions), (unsigned long long)(ai.waits - ai_.waits),
                    (unsigned long long)(ai.repeats - ai_.repeats), (unsigned long long)((ai.wait_us - ai_.wait_us) / 1000),
                    (unsigned long long)(ai.output.underruns - ai_.output.underruns));
        std::fflush(stdout);
        ai_ = ai;
    }

private:
    bool enabled_ = std::getenv("SMS_AUDIO_STATS") != nullptr;
    Uint32 last_ = 0;
    starlet::ai::Stats ai_{};
};

// Development: SMS_PERF prints, every 10 s (SMS_PERF=N: every N s), the presented frame rate and the CPU time per presented
// frame of each thread that used more than 1 ms per frame (from /proc/self/task), with the process RSS.
std::atomic<unsigned long> presented_frames{0};
class PerfStats {
public:
    void sample() {
        if (!enabled_) return;
        const Uint32 now = SDL_GetTicks();
        if (last_ == 0) {
            last_ = now;
            frames_ = presented_frames;
            read_threads(previous_);
            return;
        }
        if (now - last_ < period_ms_) return;
        const unsigned long frames = presented_frames - frames_;
        std::vector<Thread> current;
        read_threads(current);
        const double seconds = (now - last_) / 1000.0;
        std::string line;
        char item[96];
        for (const Thread& t : current) {
            unsigned long long before = 0;
            for (const Thread& p : previous_)
                if (p.tid == t.tid) before = p.ticks;
            const double ms = frames ? (t.ticks - before) * 1000.0 / ticks_per_second_ / frames : 0;
            if (ms < 1.0) continue;
            std::snprintf(item, sizeof item, " %s %.1f", t.name.c_str(), ms);
            line += item;
        }
        long rss_pages = 0;
        if (FILE* f = std::fopen("/proc/self/statm", "r")) {
            long size = 0;
            if (std::fscanf(f, "%ld %ld", &size, &rss_pages) != 2) rss_pages = 0;
            std::fclose(f);
        }
        std::printf("perf: %.1f fps, rss %ld MiB, CPU ms per frame:%s\n", frames / seconds, rss_pages * 4096 / 1048576,
                    line.c_str());
        std::fflush(stdout);
        last_ = now;
        frames_ = presented_frames;
        previous_ = current;
    }

private:
    struct Thread {
        long tid;
        std::string name;
        unsigned long long ticks;
    };
    static void read_threads(std::vector<Thread>& out) {
        out.clear();
        DIR* dir = opendir("/proc/self/task");
        if (!dir) return;
        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            char path[64], buffer[512];
            std::snprintf(path, sizeof path, "/proc/self/task/%s/stat", entry->d_name);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            const size_t n = std::fread(buffer, 1, sizeof buffer - 1, f);
            std::fclose(f);
            buffer[n] = 0;
            char* open = std::strchr(buffer, '(');
            char* close = std::strrchr(buffer, ')');
            if (!open || !close) continue;
            unsigned long long user = 0, system = 0;
            if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &user, &system) != 2) continue;
            out.push_back({std::atol(entry->d_name), std::string(open + 1, close), user + system});
        }
        closedir(dir);
    }
    bool enabled_ = std::getenv("SMS_PERF") != nullptr;
    Uint32 period_ms_ = enabled_ && std::atoi(std::getenv("SMS_PERF")) > 1 ? Uint32(std::atoi(std::getenv("SMS_PERF"))) * 1000 : 10000;
    long ticks_per_second_ = sysconf(_SC_CLK_TCK);
    Uint32 last_ = 0;
    unsigned long frames_ = 0;
    std::vector<Thread> previous_;
};

// Earlier releases kept slot A in <saves>/card_a with a card_state.txt; it becomes Dolphin's
// folder for the USA disc, where starlet::card keeps it.
void adopt_old_card(const std::string& saves) {
    const std::string old = saves + "/card_a", folder = starlet::card::folder_for(saves, starlet::card::Region::usa, 0);
    struct stat info {};
    if (stat(old.c_str(), &info) != 0 || stat(folder.c_str(), &info) == 0) return;
    mkdir((saves + "/USA").c_str(), 0755);
    if (std::rename(old.c_str(), folder.c_str()) == 0) {
        std::remove((folder + "/card_state.txt").c_str());
        std::printf("Memory card: moved %s to %s\n", old.c_str(), folder.c_str());
    }
}

// Development switches of the GPU thread: SMS_GPU_STATS (counters every 300 retraces),
// SMS_EFB_DUMP=R1,R2,... (the EFB at those retraces into SMS_CAPTURE_DIR; needs SMS_INLINE_GL),
// SMS_GX_TRACE=N and SMS_GX_TRACE_FROM=R (the state of N draws from retrace R on).
starlet::gx_fifo::Diagnostics gpu_diagnostics() {
    starlet::gx_fifo::Diagnostics d;
    d.stats = std::getenv("SMS_GPU_STATS") != nullptr;
    for (const char* p = std::getenv("SMS_EFB_DUMP"); p && *p;) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(p, &end, 10);
        if (end == p) break;
        d.efb_dumps.push_back(unsigned(value));
        p = *end == ',' ? end + 1 : end;
    }
    if (const char* dir = std::getenv("SMS_CAPTURE_DIR")) d.capture_dir = dir;
    if (const char* n = std::getenv("SMS_GX_TRACE")) d.trace_draws = std::atoi(n);
    if (const char* r = std::getenv("SMS_GX_TRACE_FROM")) d.trace_from = unsigned(std::atoi(r));
    return d;
}

// The program list shipped beside the executable.
std::string shipped_program_list() {
    char path[4096];
    const auto size = readlink("/proc/self/exe", path, sizeof path - 1);
    if (size <= 0) return {};
    const std::string exe(path, static_cast<std::size_t>(size));
    return exe.substr(0, exe.rfind('/') + 1) + "programs.list";
}

// Builds the listed programs the cache lacks before the game starts, behind a
// progress bar (first launch, new lists, driver updates). A Mali link takes up to
// about a second; beside the running game it stalls the GL thread, the GPU thread
// waiting on it and the game's draw-done waits. Programs missing from every list
// then build in the background while their draws are skipped.
void prepare_programs(starlet::gx::GlesRenderer& renderer, SDL_Window* window) {
    auto s = renderer.program_builder();
    while (s.running && !s.planned) {
        SDL_Delay(10);
        s = renderer.program_builder();
    }
    if (s.running && s.to_build) {
        const Uint32 started = SDL_GetTicks();
        std::printf("Preparing %zu shaders\n", s.to_build);
        std::fflush(stdout);
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(window, &width, &height);
        const int bar_width = width * 3 / 4, bar_height = std::max(height / 40, 4);
        const int x = (width - bar_width) / 2, y = (height - bar_height) / 2;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        for (; s.running && !s.lists_done; s = renderer.program_builder()) {
            starlet::host::poll_events(overlay::handle);
            if (starlet::host::quit_requested()) {
                std::printf("Exit requested by the frontend\n");
                starlet::host::exit_now(0);
            }
            const int done = static_cast<int>(std::int64_t(bar_width) * s.finished / s.to_build);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, bar_width, bar_height);
            glClearColor(0.25f, 0.25f, 0.25f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if (done > 0) {
                glScissor(x, y, done, bar_height);
                glClearColor(1, 1, 1, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            overlay::frame(width, height);
            SDL_GL_SwapWindow(window);
            SDL_Delay(30);
        }
        std::printf("Prepared %zu shaders in %.1f s (%zu failed)\n", s.built, (SDL_GetTicks() - started) / 1000.0, s.failed);
        std::fflush(stdout);
    }
    renderer.build_programs_in_background();
}

// A debugger breakpoint for the end of a timed run (thread backtraces of a hang).
__attribute__((noinline)) void run_time_over() { asm volatile(""); }

// An instrumented build (-fprofile-generate, for PGO) writes its profile at exit; the exits here
// end with _Exit, so they write it themselves. Other builds lack the symbol.
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));
void write_instrumented_profile() {
    if (__llvm_profile_write_file) __llvm_profile_write_file();
}

// Development: SMS_PROFILE (Starlet's profiler) and SMS_FRAME_TRACE write into SMS_CAPTURE_DIR.
std::string capture_dir() {
    const char* dir = std::getenv("SMS_CAPTURE_DIR");
    return dir ? dir : "/tmp";
}
void start_profiler() {
    if (std::getenv("SMS_FRAME_TRACE")) starlet::gx_fifo::start_frame_trace();
    starlet::os::start_profiler("SMS");
}
void write_profile() {
    if (std::getenv("SMS_FRAME_TRACE")) starlet::gx_fifo::write_frame_trace((capture_dir() + "/sms-frames.txt").c_str());
    starlet::os::write_profile(capture_dir().c_str());
}

std::string argument(int argc, char** argv, int index, const char* fallback) {
    return index < argc ? argv[index] : fallback;
}

}

namespace sms::application {

int run_checked(int argc, char** argv, int (*guest_main)());

int run(int argc, char** argv, int (*guest_main)()) {
    try {
        return run_checked(argc, argv, guest_main);
    } catch (const std::exception& error) {
        // Threads still hold the renderer and the display: report and leave without
        // running their destructors.
        std::fprintf(stderr, "sunshine: %s\n", error.what());
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

int run_checked(int argc, char** argv, int (*guest_main)()) {
    const std::string gamedata = argument(argc, argv, 1, "gamedata");
    const std::string saves = argument(argc, argv, 2, "saves");
    const std::string shadercache = argument(argc, argv, 3, "shadercache");
    // The arena goes first, before the GPU driver and SDL map anything low.
    sms::platform::check_sdk_layout();
    sms::platform::initialize_memory();
    Display display;
    // The frontend's SIGTERM: card writes in progress finish, then the process exits within 1 s.
    starlet::host::install_quit_signals();
    starlet::host::on_quit([] {
        starlet::card::finish_card_writes(std::chrono::milliseconds(400));
        overlay::save();
        write_instrumented_profile();
    });
    starlet::host::exit_after_quit(std::chrono::milliseconds(500));
    std::printf("GL renderer: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    overlay::init();
    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(display.window, &width, &height);
    drawable_width = width;
    drawable_height = height;
    std::printf("Display: %dx%d\n", width, height);

    starlet::gx::GlesLimits limits;
    limits.efb.height = 448;
    limits.aspect_width = 4;
    limits.aspect_height = 3;
    limits.direct.shading = starlet::gx::DirectShading::fast;
    // 2D screens (2-Player Battle's menus) scissor past their viewport: draw them clipped to
    // the viewport instead of refusing them.
    limits.direct.guard_band = starlet::gx::GuardBand::clip_to_viewport;
    // The pod's treasure collection movie moves its viewport by 47.2 rows; refused, the
    // treasure model would not draw.
    limits.direct.fractional_viewport = starlet::gx::FractionalViewport::round;
    limits.direct.program_cache = shadercache;
    // EFB copy destinations are section-heap buffers that the heap hands out again after the
    // section ends; JKRDecompressFromDVDToAram fills such memory with the CPU and then sends it
    // to ARAM, which would otherwise write a dead copy's texels over it. On the R36S cave
    // stamps cost no measurable frame time.
    limits.direct.stamp_resident_copies = true;
    // Draw-sync callbacks test single EFB pixels (the sun's depth, Mario's alpha): full-resolution snapshots.
    limits.direct.snapshot_scale = 1;
    // Programs missing during play (Starlet GlesDirectLimits). On muos_35-h from an empty cache,
    // two normal-priority workers and a GL-thread link after 50 ms, at most every 250 ms, skipped
    // 4.5 times fewer draws than Starlet's defaults with similar long frames.
    limits.direct.unready_link_ms = 50;
    limits.direct.unready_link_interval_ms = 250;
    limits.direct.builder_policy.request_workers = 2;
    limits.direct.builder_policy.normal_priority = true;
    // Development: the SMS_ switches below override that policy.
    if (const char* ms = std::getenv("SMS_UNREADY_LINK_MS")) limits.direct.unready_link_ms = unsigned(std::atoi(ms));
    if (const char* ms = std::getenv("SMS_UNREADY_LINK_INTERVAL_MS")) limits.direct.unready_link_interval_ms = unsigned(std::atoi(ms));
    if (const char* n = std::getenv("SMS_REQUEST_WORKERS")) limits.direct.builder_policy.request_workers = unsigned(std::atoi(n));
    if (const char* on = std::getenv("SMS_REQUEST_NORMAL_PRIORITY")) limits.direct.builder_policy.normal_priority = on[0] != '0';
    limits.direct.builder_policy.newest_first = std::getenv("SMS_REQUEST_NEWEST_FIRST") != nullptr;
    // Development: SMS_GEOMETRY_MIN keeps smaller indexed draws resident (instanceable).
    if (const char* n = std::getenv("SMS_GEOMETRY_MIN")) limits.direct.geometry_min_vertices = unsigned(std::atoi(n));
    // Recorded draws go to the GL thread every 256 KiB, so it submits while the game still
    // builds the frame; the game's draw-done wait then covers less GL work (Dweevil Nest:
    // late frames halved). Development: SMS_SEGMENT_KB sets another size.
    limits.direct.segment_bytes = 256 * 1024;
    if (const char* kb = std::getenv("SMS_SEGMENT_KB")) limits.direct.segment_bytes = std::size_t(std::atoi(kb)) * 1024;
    // This device's own list first: what its player has already reached.
    limits.direct.program_lists = {shadercache + "/programs.list", shipped_program_list()};
    starlet::gx::GlesRenderer renderer(limits);
    prepare_programs(renderer, display.window);
    shared_captures();
    renderer.set_swap([&display] {
        overlay::frame(drawable_width, drawable_height);
        shared_captures().before_swap();
        SDL_GL_SwapWindow(display.window);
        ++presented_frames;
        starlet::gx_fifo::frame_event('W');
        starlet::os::profile_frame();
    });
    SDL_GL_MakeCurrent(display.window, nullptr);

    static starlet::gx_fifo::GlesBackend backend(renderer);
    starlet::gx_fifo::GpuSetup gpu;
    gpu.renderer = &backend;
    gpu.efb = limits.efb;
    gpu.flush_dirty_state = sunshine_gx_flush_dirty_state;
    // sms-pc-port's converters leave the vertex data the game loads in host order (src/graphics).
    gpu.arrays_host_order = true;
    gpu.diagnostics = gpu_diagnostics();
    // Development: SMS_INLINE_GL runs GL on the GPU thread itself instead of a submission thread.
    static const bool inline_gl = std::getenv("SMS_INLINE_GL") != nullptr;
    gpu.start = [&] {
        if (inline_gl) {
            SDL_GL_MakeCurrent(display.window, display.gl);
            renderer.adopt_thread();
            return;
        }
        renderer.adopt_thread();
        renderer.start_submission([] {},
                                  [&display] {
                                      starlet::os::name_thread("sms-gl");
                                      SDL_GL_MakeCurrent(display.window, display.gl);
                                  },
                                  [&display] {
                                      overlay::shutdown();
                                      SDL_GL_MakeCurrent(display.window, nullptr);
                                  });
    };
    gpu.stop = [&renderer] {
        if (!inline_gl) renderer.stop_submission();
    };
    gpu.drawable_size = [](int& w, int& h) {
        w = drawable_width;
        h = drawable_height;
    };
    starlet::gx_fifo::start_gpu(gpu);

    starlet::dvd::initialize({{gamedata + "/sys", {gamedata + "/files"}}});
    adopt_old_card(saves);
    starlet::card::initialize_cards(saves);
    // Development: SMS_AUDIO_CAPTURE records what the audio DMA plays to that WAV file,
    // SMS_GAME_LOG prints the game's OSReport text.
    if (const char* path = std::getenv("SMS_AUDIO_CAPTURE")) starlet::ai::set_capture(path);
    starlet::os::set_game_log(std::getenv("SMS_GAME_LOG") != nullptr);
    starlet::gx_fifo::set_video_presenter(starlet::gx_fifo::present);

    start_profiler();
    AudioStats audio_stats;
    PerfStats perf_stats;
    Controller controller;
    controller.sample();
    std::atomic<bool> game_returned{false};
    std::thread guest([&] {
        starlet::os::name_thread("sms-guest-main");
        starlet::os::run_main_thread(guest_main);
        game_returned = true;
    });
    guest.detach();
    // Development: SMS_RUN_SECONDS ends a timed run as the frontend would.
    const char* run_seconds = std::getenv("SMS_RUN_SECONDS");
    const Uint32 deadline = run_seconds ? SDL_GetTicks() + Uint32(std::atoi(run_seconds)) * 1000 : 0;
    // The session's shader misses, for tester logs: draws skipped while their program built,
    // and programs the GL thread linked itself (each a hitch of that link's length).
    auto summarize = [&renderer] {
        const auto s = renderer.stats().direct;
        std::printf("Shaders during play: %llu draws skipped while building, %llu linked on the GL thread (longest %llu ms)\n",
                    (unsigned long long)s.unready_draws, (unsigned long long)s.fallback_links,
                    (unsigned long long)s.fallback_link_max_ms);
    };
    while (!game_returned) {
        if (deadline && SDL_TICKS_PASSED(SDL_GetTicks(), deadline)) {
            std::printf("Run time over\n");
            summarize();
            run_time_over();
            write_profile();
            write_instrumented_profile();
            std::fflush(nullptr);
            std::_Exit(0);
        }
        starlet::host::poll_events(overlay::handle);
        if (starlet::host::quit_requested()) {
            std::printf("Exit requested by the frontend\n");
            summarize();
            write_profile();
            starlet::host::exit_now(0);
        }
        controller.sample();
        overlay::save_when_closed();
        audio_stats.sample();
        perf_stats.sample();
        SDL_Delay(4);
    }
    std::printf("The game's main() returned\n");
    return 0;
}

const char* build_id() { return starlet::os::build_id(); }

}
