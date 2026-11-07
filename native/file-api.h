#ifndef MOVIES_FILE_API_H
#define MOVIES_FILE_API_H
#include "runtime.h"
/* Absolute existing backend roots; disjoint directories. Original game is never
 * opened for writing. Initialize before guest execution, not concurrently. */
int movies_file_init(const char *game_root,const char *userdata_root);
/* Logical-file quota for private userdata, default 256 MiB. Sparse gaps and
 * COW copies count; original assets do not. Browser userdata is volatile MEMFS.
 * Configure before init (or lower at runtime; existing files remain readable).
 * Private backend writes must go through this module; queries rescan to reconcile
 * external changes. RAM allocation and physical filesystem errors can still fail. */
void movies_file_set_quota(uint64_t bytes);
/* Metadata-only read-only System32 entry, gated by startup module/export query.
 * Size zero means no Windows PE backing; opens and deletion are denied. */
int movies_file_register_builtin_module(const char *dll,const char *required_export);
/* Optional non-file CloseHandle resolver: 1 closed, 0 not owned. */
void movies_file_set_close_resolver(int (*resolver)(uint32_t));
/* DeleteFile supports closed private files only. Original-backed names deny
 * deletion (no tombstones); open SHARE_DELETE files return unsupported (50). */
/* GetFullPathName is lexical only (ASCII A/W subset, no device namespaces);
 * resolving a Windows path never authorizes backend access to that path. */
ImportHandler *movies_file_handlers(uint32_t *count);
#endif
