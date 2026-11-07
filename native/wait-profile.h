#ifndef MOVIES_WAIT_PROFILE_H
#define MOVIES_WAIT_PROFILE_H
#include <stdint.h>
#define MOVIES_WAIT_PROFILE_SLOTS 17
typedef struct MoviesWaitProfileEntry {
 uint32_t kind,caller,timeout,status;uint64_t count,ns,max_ns;
} MoviesWaitProfileEntry;
typedef struct MoviesWaitProfileReport {
 uint32_t frames,last_serial,started;uint64_t window_ns;
 MoviesWaitProfileEntry entries[2][MOVIES_WAIT_PROFILE_SLOTS];
} MoviesWaitProfileReport;
typedef void (*MoviesWaitProfileSink)(const MoviesWaitProfileReport*);
void movies_wait_profile_init(int enabled,MoviesWaitProfileSink sink);
int movies_wait_profile_active(void);
uint64_t movies_wait_profile_begin(void);
void movies_wait_profile_record(uint32_t kind,uint32_t caller,uint32_t timeout,uint32_t status,uint64_t begin);
void movies_wait_profile_present(void);
void movies_wait_profile_flush(void);
/* Deterministic aggregation test seam; production record supplies monotonic end. */
void movies_wait_profile_interval(uint32_t kind,uint32_t caller,uint32_t timeout,uint32_t status,uint64_t begin,uint64_t end);
#endif
