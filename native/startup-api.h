#ifndef MOVIES_STARTUP_API_H
#define MOVIES_STARTUP_API_H
#include "runtime.h"
ImportHandler *movies_startup_handlers(uint32_t image_base, uint32_t *count);
/* Host opt-in after handlers are installed: registers a virtual DLL only if
 * its required export is genuinely implemented. No disk DLL loading or DllMain.
 * Then LoadLibrary acquires references; GetModuleHandle observes loaded state. */
int movies_startup_register_module(const char *dll,const char *required_export);
/* Registered identity + exact registration export + currently installed handler. */
int movies_startup_module_available(const char *dll,const char *required_export);
void movies_set_last_error(uint32_t value);
uint32_t movies_get_last_error(void);
uint32_t movies_unhandled_exception_filter(void);
#endif
