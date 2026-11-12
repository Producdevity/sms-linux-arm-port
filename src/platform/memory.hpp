#pragma once

namespace sms::platform {

// Super Mario Sunshine's memory on starlet::os. The game's memory sits at the console's own
// addresses: MEM1 at 0x80000000 (physical address p is native address 0x80000000 + p), 64 MiB
// because native objects are larger than the console's, with the low-memory globals in its first
// 16 KiB and the arena after them. Guest stacks come from a pool below 2 GiB. Then the console's
// boot state for GMSE01. Call first, before the GPU driver and SDL map anything low.
void initialize_memory();

}
