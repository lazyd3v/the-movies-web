#ifndef MOVIES_SHELL_API_H
#define MOVIES_SHELL_API_H
#include "runtime.h"
/* Initialize a private backend; creates the profile Documents, AppData/Roaming and
 * AppData/Local directories initially, matching a provisioned Windows user.
 * No host home-directory discovery. Call once before guest execution. */
int movies_shell_init(const char *userdata_root);
/* Exact CSIDL mappings (no flags). Helpers do not verify existence or authorize I/O. */
const char *movies_shell_virtual_path(uint32_t csidl);
int movies_shell_backend_path(uint32_t csidl,char *out,size_t capacity);
ImportHandler *movies_shell_handlers(uint32_t *count);
#endif
