#ifndef MOVIES_REGISTRY_API_H
#define MOVIES_REGISTRY_API_H
#include "runtime.h"
/* Private, process-lifetime registry. No host registry access or persistence.
 * userdata_root is reserved, never opened. Call init only while guest is stopped.
 * ACP=1252, names case-insensitive in the text module's Western repertoire.
 * One 32-bit view; no ACL/security descriptors, links, remote or performance keys.
 * Standard handle masks are stored; QUERY_VALUE/SET_VALUE are enforced.
 * Nonvolatile/volatile creation both last for this runtime instance only. */
void movies_registry_init(const char *userdata_root);
ImportHandler *movies_registry_handlers(uint32_t *count);
#endif
