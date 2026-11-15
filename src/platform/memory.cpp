#include "platform/memory.hpp"

#include "starlet/os/game_memory.hpp"
#include "starlet/os/os_core.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace sms::platform {
namespace {

constexpr std::uintptr_t mem1_base = 0x80000000;
constexpr std::size_t mem1_size = std::size_t{64} << 20;
// The console's low memory (boot block, OS globals, exception vectors) up to the first arena byte.
constexpr std::size_t low_memory_size = 0x4000;
constexpr std::size_t stack_pool_size = std::size_t{96} << 20;
constexpr std::uintptr_t arena_alignment = 0x10000;

std::uintptr_t round_up(std::uintptr_t value, std::uintptr_t unit) { return (value + unit - 1) / unit * unit; }

}

void initialize_memory() {
    std::uintptr_t begin = 0, end = 0;
    starlet::os::image_range(begin, end);
    if (end > mem1_base) throw std::runtime_error("the executable image ends above 2 GiB");
    starlet::os::set_memory_map({mem1_base});
    void* low = starlet::os::map_fixed(mem1_base, low_memory_size, "MEM1 low memory");
    // The image's physical addresses (0x200000 up) name MEM1 above the low memory; the GX writes
    // statics through them, so the arena starts after that range. It stays mapped for the host's
    // reads of game state.
    std::uintptr_t alias_begin = 0, alias_end = 0;
    starlet::os::image_alias(alias_begin, alias_end);
    const std::uintptr_t arena_base = alias_end > mem1_base + low_memory_size ? round_up(alias_end, arena_alignment)
                                                                             : mem1_base + low_memory_size;
    void* rest = mmap(reinterpret_cast<void*>(mem1_base + low_memory_size), mem1_size - low_memory_size,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (rest != reinterpret_cast<void*>(mem1_base + low_memory_size))
        throw std::runtime_error(std::string("cannot map MEM1 at 0x80004000: ") + std::strerror(errno));
    starlet::os::set_arena(reinterpret_cast<void*>(arena_base), mem1_base + mem1_size - arena_base);
    // JKRAram and the audio driver DMA into buffers on guest stacks, so the stacks are game memory too.
    const std::uintptr_t hints[] = {0x40000000, 0x48000000, 0x50000000, 0x58000000, 0x60000000};
    starlet::os::set_stack_pool(stack_pool_size, mem1_base, hints, sizeof hints / sizeof hints[0]);
    starlet::os::OsConfig config;
    std::memcpy(config.game_id, "GMSE01", 6);
    config.low_memory = low;
    starlet::os::initialize_os(config);
}

}
