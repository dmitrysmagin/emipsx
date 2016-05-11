#ifndef __PSXPORT_H__
#define __PSXPORT_H__

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <assert.h>

#if defined(ANDROID) || defined(__SYMBIAN32__)
#include "div.h"
#else
#define UDIV(n,d) ((n)/(d))
#define SDIV(n,d) ((n)/(d))
#endif

extern unsigned get_ticks(void);
extern void wait_ticks(unsigned s);
extern void pad_update(void);
extern unsigned short pad_read(int num);

//senquack - spu_pcsxrearmed has its own sound backends
#ifndef spu_pcsxrearmed
extern void sound_init(void);
extern void sound_close(void);
extern unsigned long sound_get(void);
extern void sound_set(unsigned char *pSound, long lBytes);
#endif //spu_pcsxrearmed

extern unsigned short* video_get_screenptr();   //senquack - added
extern void video_flip(void);
//senquack - TODO: remove video_set() or adapt it
extern void video_set(unsigned short* pVideo,unsigned int width,unsigned int height);
extern void video_clear(void);
extern void pcsx4all_exit(void);
extern void port_printf(int x,int y,char *text);
extern void port_sync(void);
extern void port_mute(void);

#endif
