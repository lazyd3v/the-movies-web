#ifndef MOVIES_ACM_API_H
#define MOVIES_ACM_API_H
#include "runtime.h"
/* MSACM32 acmStream*: MPEG Layer II (0x50) and identical PCM16 to PCM16. */
ImportHandler *movies_acm_handlers(uint32_t *count);
#endif
