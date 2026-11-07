#ifndef MOVIES_DIALOG_API_H
#define MOVIES_DIALOG_API_H
#include "runtime.h"
/* Synchronous callback receives valid UTF8 strings and original Win32 flags.
 * It must return an actual supported button ID. Set before guest execution;
 * lifetime belongs to host. No callback means strict trap on native builds. */
typedef int (*MoviesDialogCallback)(const char *title,const char *text,uint32_t flags);
void movies_dialog_set_callback(MoviesDialogCallback callback);
ImportHandler *movies_dialog_handlers(uint32_t *count);
#endif
