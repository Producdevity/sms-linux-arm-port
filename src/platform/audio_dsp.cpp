// The console's DSP for JAudio: sms-pc-port's software mixer (platform/audio/dsp_hle.cpp and
// dsp_mixer.cpp, MIT) on Starlet's interrupts and ARAM. The audio interface that plays the mixed
// blocks is starlet::ai.
#include "starlet/ar/ar.hpp"
#include "starlet/os/guest_cpu.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace {

using PollFn = void (*)();
std::mutex poll_mutex;
std::vector<PollFn> polls;

void run_polls() {
    std::vector<PollFn> copy;
    {
        std::lock_guard<std::mutex> lock(poll_mutex);
        copy = polls;
    }
    for (PollFn fn : copy) fn();
}

}

// Work to run in interrupt context at the game's next check point (the caller holds the CPU).
void port_irq_defer(std::function<void()> fn) { starlet::os::raise_interrupt(std::move(fn)); }

extern "C" {

// The mixer thread finished a subframe: its poll delivers the mail at the next interrupt.
void port_irq_kick(void) { starlet::os::raise_interrupt(run_polls); }
void port_irq_add_source(PollFn fn) {
    std::lock_guard<std::mutex> lock(poll_mutex);
    polls.push_back(fn);
}
// The VI runs on the host clock; the mixer may take its own thread.
int port_vi_deterministic(void) { return 0; }

// ARAM byte address to host memory (starlet::ar's 16 MiB).
std::uint8_t* port_aram_ptr(std::uint32_t address) {
    return address < starlet::ar::bytes() ? starlet::ar::memory() + address : nullptr;
}

int port_dspmix_class_gain(int cls);
// THP movie audio mixed on the CPU (patch audio-05): the music class's gain in Q15.
int port_audio_music_gain_q15(void) { return port_dspmix_class_gain(1); }

}
