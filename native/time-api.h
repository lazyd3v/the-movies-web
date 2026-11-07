#ifndef MOVIES_TIME_API_H
#define MOVIES_TIME_API_H
#include "runtime.h"
/* Explicit virtual UTC timezone with no daylight-saving transitions. Calendar
 * conversion uses Gregorian dates and100ns ticks since1601, never host TZ. */
ImportHandler *movies_time_handlers(uint32_t *count);
#endif
