#include "game/game_heap.hpp"

#include "starlet/os/host_allocation.hpp"

// The game's own operator new and delete over JKRHeap, renamed in the game library
// (cmake/Decomp.cmake), and JKRHeap::sCurrentHeap, the heap they allocate from.
extern "C" void* __sms_game_new(unsigned long size);
extern "C" void __sms_game_delete(void* pointer);
extern "C" void* sms_jkr_current_heap __asm__("_ZN7JKRHeap12sCurrentHeapE");

namespace sms::game {

void install_game_heap() {
    starlet::os::GameHeap heap;
    heap.ready = [] { return sms_jkr_current_heap != nullptr; };
    heap.allocate = [](std::size_t size) -> void* { return __sms_game_new(static_cast<unsigned long>(size)); };
    heap.release = [](void* pointer) { __sms_game_delete(pointer); };
    starlet::os::set_game_heap(heap);
}

}
