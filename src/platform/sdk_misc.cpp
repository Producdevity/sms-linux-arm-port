// SDK and sms-pc-port runtime calls with no Starlet module behind them.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" {

// The disc is always present: the game reads it from gamedata/.
int DVDCheckDisk(void) { return 1; }

// sms-pc-port's log, used by its converters and mixer.
void port_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fflush(stdout);
}

// sms-pc-port's dolphin/types.h stores pointers in 32-bit slots and calls this when one does not fit.
void port_ptr32_trap(const void* pointer) {
    std::fprintf(stderr, "sunshine: address %p does not fit a 32-bit pointer slot\n", pointer);
    std::fflush(nullptr);
    std::abort();
}

// Movies always play (sms-pc-port's THPPlayer patch 0016 can skip them).
int port_skip_movies = 0;
// SMS_NO_AUDIO in sms-pc-port: the empty sound configuration is never used here.
int port_no_audio = 0;
unsigned char* port_noaudio_init_data(void) { return nullptr; }

// JAudio's DSP task names its microcode by this address; the software DSP does not read it.
unsigned char lbl_80004600[4];

}
