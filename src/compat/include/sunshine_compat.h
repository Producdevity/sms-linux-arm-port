/* Force-included after sms-pc-port's port_compat.h: the renderer calls sms-pc-port's patches
 * make, served here by src/graphics over Starlet. */
#ifndef SUNSHINE_COMPAT_H
#define SUNSHINE_COMPAT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Retail bytes the GX reads big-endian although the game's other vertex arrays are host order. */
void GXPC_AddBigEndianRange(const void* base, unsigned long bytes);
#ifdef __cplusplus
}
#endif
#endif
