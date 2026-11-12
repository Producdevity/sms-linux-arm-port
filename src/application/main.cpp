// The Super Mario Sunshine executable: the host application running the game's main().
#include "application/host_app.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>

// The game's void main(void), renamed by sms-pc-port's port_compat.h.
void SMS_main();
extern const char sunshine_port_version[];

namespace {
int guest_main() {
    SMS_main();
    return 0;
}
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("Super Mario Sunshine port %s, build %s\n", sunshine_port_version, sms::application::build_id());
    int status = 0;
    try {
        status = sms::application::run(argc, argv, guest_main);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "sunshine: %s\n", error.what());
        status = 1;
    }
    // The game's static objects point into its arena; the console never destroys them, so
    // neither does this process.
    std::fflush(nullptr);
    std::_Exit(status);
}
