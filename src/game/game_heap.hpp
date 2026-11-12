#pragma once

namespace sms::game {

// Routes the game's operator new and delete to JKRHeap once the game made its root heap, as the
// console build does (starlet::os dispatches the global operators).
void install_game_heap();

}
