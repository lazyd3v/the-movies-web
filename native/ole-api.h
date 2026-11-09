#ifndef MOVIES_OLE_API_H
#define MOVIES_OLE_API_H
#include "runtime.h"
/* OLE32/OLEAUT32 helpers outside the media COM objects: BSTR and VARIANT
 * lifetime, running-object-table/moniker refusal and library unloading.
 * BSTRs live in private backend memory: 4-byte byte length, UTF-16, NUL. */
ImportHandler *movies_ole_handlers(uint32_t *count);
#endif
