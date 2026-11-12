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

}

void initialize_memory() {
    starlet::os::set_memory_map({mem1_base});
    void* low = starlet::os::map_fixed(mem1_base, low_memory_size, "MEM1 low memory");
    void* arena = mmap(reinterpret_cast<void*>(mem1_base + low_memory_size), mem1_size - low_memory_size,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (arena != reinterpret_cast<void*>(mem1_base + low_memory_size))
        throw std::runtime_error(std::string("cannot map MEM1 at 0x80004000: ") + std::strerror(errno));
    starlet::os::set_arena(arena, mem1_size - low_memory_size);
    std::uintptr_t begin = 0, end = 0;
    starlet::os::image_range(begin, end);
    if (end > mem1_base) throw std::runtime_error("the executable image ends above 2 GiB");
    // JKRAram and the audio driver DMA into buffers on guest stacks, so the stacks are game memory too.
    const std::uintptr_t hints[] = {0x40000000, 0x48000000, 0x50000000, 0x58000000, 0x60000000};
    starlet::os::set_stack_pool(stack_pool_size, mem1_base, hints, sizeof hints / sizeof hints[0]);
    starlet::os::OsConfig config;
    std::memcpy(config.game_id, "GMSE01", 6);
    config.low_memory = low;
    starlet::os::initialize_os(config);
}

}
