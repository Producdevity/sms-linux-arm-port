#pragma once

namespace sms::application {

// The console underneath a guest program: SDL window and OpenGL ES context,
// Starlet's renderer on the GPU thread, audio, controller sampling on this
// thread, and guest_main on the guest CPU. Returns the process status.
int run(int argc, char** argv, int (*guest_main)());

// The executable's GNU build ID, as fatal-signal reports print it.
const char* build_id();

}
