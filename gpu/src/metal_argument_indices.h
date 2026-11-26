#ifndef REDWOOD_METAL_ARGUMENT_INDICES_H
#define REDWOOD_METAL_ARGUMENT_INDICES_H
#include <stdint.h>
#include <stdbool.h>
/* Descriptor IDs may have gaps. Resource records are keyed by ID; storage
 * spans the highest ID, not the number of descriptors. Bound before shifting. */
static inline bool rw_argument_index_add(uint64_t *seen, unsigned *length, uint64_t index)
{
    if (index >= 64 || (*seen & (UINT64_C(1) << index))) return false;
    *seen |= UINT64_C(1) << index;
    unsigned end = (unsigned)(index + 1) * 8;
    if (*length < end) *length = end;
    return true;
}
#endif
