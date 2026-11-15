/* The address -> lifted function table (generated as private/lifted/lifted_table.c, sorted by address).
 *
 * What it is for: usf_icall looks a guest code address up here (binary search) for indirect calls, virtual calls and
 * callbacks. What it does not do: imports; those are answered by the runtime's own table (thunk addresses).
 */
#ifndef USF_TABLE_H
#define USF_TABLE_H

#include <stdint.h>

#include "cpu.h"

typedef struct {
    uint32_t addr;
    void (*fn)(CPU *c);
} USFEntry;

extern const USFEntry usf_table[];
extern const unsigned usf_table_count;

#endif
