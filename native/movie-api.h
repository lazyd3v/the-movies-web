#ifndef MOVIES_MOVIE_API_H
#define MOVIES_MOVIE_API_H
#include "runtime.h"
/* Guest memory for movie playback objects (4 blocks of 0x400 + vtables). */
#define MOVIES_MOVIE_BASE 0x04014000u
int movies_movie_init(void);
/* CoCreateInstance(CLSID_WMAsfReader): 1 with *out = IBaseFilter, 0 not ours, else HRESULT. */
uint32_t movies_movie_create_reader(uint32_t clsid, uint32_t *out);
int movies_movie_is_reader_clsid(uint32_t clsid);
/* IFilterGraph hooks for playback graphs (graphs given a non-writer filter). */
uint32_t movies_movie_add_filter(CPU *c, uint32_t graph, uint32_t filter, uint32_t name);
int movies_movie_owns_graph(uint32_t graph);
uint32_t movies_movie_query(uint32_t graph, uint32_t iid, uint32_t out);
uint32_t movies_movie_render(CPU *c, uint32_t graph, uint32_t pin);
void movies_movie_graph_release(CPU *c, uint32_t graph);
#endif
