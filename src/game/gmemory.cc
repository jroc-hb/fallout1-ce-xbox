#include "game/gmemory.h"

#include "int/memdbg.h"
#include "plib/assoc/assoc.h"
#include "plib/db/db.h"
#include "plib/gnw/memory.h"

#ifdef NXDK
//debug logging
#include <xboxkrnl/xboxkrnl.h>
#endif

namespace fallout {

// 0x442E40
int gmemory_init()
{
    DbgPrint("Entering assoc_register_mem\n");
    assoc_register_mem(mem_malloc, mem_realloc, mem_free);
    DbgPrint("assoc_register_mem done\n");
    db_register_mem(mem_malloc, mem_strdup, mem_free);
    DbgPrint("db_register_mem done\n");
    memoryRegisterAlloc(gmalloc, grealloc, gfree);
    DbgPrint("gmemory_init done\n");
    return 0;
}

// 0x442E84
void* gmalloc(size_t size)
{
    return mem_malloc(size);
}

// 0x442E8C
void* grealloc(void* ptr, size_t newSize)
{
    return mem_realloc(ptr, newSize);
}

// 0x442E94
void gfree(void* ptr)
{
    mem_free(ptr);
}

} // namespace fallout
