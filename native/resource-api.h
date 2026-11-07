#ifndef MOVIES_RESOURCE_API_H
#define MOVIES_RESOURCE_API_H
#include "runtime.h"
/* Views refer to the already-loaded immutable PE image, valid until reinit.
 * Initialize after load_image and before guest threads. No rendering implied. */
typedef struct MoviesIconImage { uint32_t guest_address,bytes;uint16_t width,height,planes,bpp,id; } MoviesIconImage;
typedef struct MoviesIconResource { uint32_t handle;uint16_t id,language,count,selected;const MoviesIconImage *images; } MoviesIconResource;
typedef struct MoviesCursorResource { uint32_t system_id;const char *css_name; } MoviesCursorResource;
typedef struct MoviesBrushResource { uint32_t style,color; } MoviesBrushResource;
int movies_resource_init(uint32_t image_base);
const MoviesIconResource *movies_resource_icon(uint32_t handle);
const MoviesCursorResource *movies_resource_cursor(uint32_t handle);
const MoviesBrushResource *movies_resource_brush(uint32_t handle);
ImportHandler *movies_resource_handlers(uint32_t *count);
#endif
