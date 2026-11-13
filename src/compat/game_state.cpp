// Development: the application state, current area and file-select progress, read by the host's
// press script from its own thread.
#include <GC2D/CardLoad.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>

// The director and its TCardLoad live in MEM1 (0x80000000, 64 MiB, always mapped); this thread
// can see them half-built or freed, so it reads them only through pointers inside MEM1.
static bool in_mem1(const void* p, unsigned long size)
{
	const unsigned long a = (unsigned long)p;
	return a >= 0x80000000ul && a + size <= 0x84000000ul && a % sizeof(void*) == 0;
}

// Bits 24-31: TCardLoad progress + 1 (0 without one); 16-23: TApplication app state;
// 8-15: stage; 0-7: scenario.
extern "C" unsigned sunshine_game_state()
{
	const unsigned state = __atomic_load_n(&gpApplication.mAppState, __ATOMIC_RELAXED);
	const unsigned stage = __atomic_load_n(&gpApplication.mCurrArea.unk0, __ATOMIC_RELAXED);
	const unsigned scenario = __atomic_load_n(&gpApplication.mCurrArea.unk1, __ATOMIC_RELAXED);
	unsigned progress = 0;
	if (state == TApplication::APP_STATE_GAMEPLAY && stage == 15) {
		TMarDirector* director = __atomic_load_n(&gpMarDirector, __ATOMIC_RELAXED);
		TCardLoad* load = in_mem1(director, sizeof(TMarDirector))
		                      ? __atomic_load_n(&director->unk70, __ATOMIC_RELAXED) : nullptr;
		if (in_mem1(load, sizeof(TCardLoad))) progress = (unsigned(__atomic_load_n((int*)&load->unk1C, __ATOMIC_RELAXED)) + 1) & 0xff;
	}
	return progress << 24 | state << 16 | stage << 8 | scenario;
}

// Development: a stage change requested by the host's press script, made on the game thread from
// its PADRead through the director's own setNextStage (code: (stage + 1) << 8 | scenario).
static int warp_code;

extern "C" void sunshine_request_warp(int code)
{
	__atomic_store_n(&warp_code, code, __ATOMIC_RELEASE);
}

extern "C" void sunshine_pad_read_hook()
{
	const int code = __atomic_load_n(&warp_code, __ATOMIC_ACQUIRE);
	if (!code || gpApplication.mAppState != TApplication::APP_STATE_GAMEPLAY || !gpMarDirector)
		return;
	__atomic_store_n(&warp_code, 0, __ATOMIC_RELAXED);
	gpMarDirector->setNextStage(code, nullptr);
}
