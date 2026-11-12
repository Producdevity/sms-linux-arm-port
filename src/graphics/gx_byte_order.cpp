// Vertex array byte order. sms-pc-port's converters swap the vertex data the game loads to host
// order (its platform/endian), and arrays the game fills natively are host order anyway, so Starlet
// reads every array as host order (GpuSetup::arrays_host_order); the few retail bytes left
// big-endian are registered here.
#include "starlet/compat/gx_fifo.h"

extern "C" void GXPC_AddBigEndianRange(const void* base, unsigned long bytes) { starlet_gx_big_endian_array(base, bytes); }
