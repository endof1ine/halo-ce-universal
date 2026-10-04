/*
GUEST_MEMCMP.C

memcmp a word at a time, in place of musl's, which compares a byte at a
time: the renderer compares a few hundred bytes of state at each draw
(d3d8_gl.c's pixel shader key and draw uniforms), which was 3% of the game's
thread on the Switch, and the game compares its tags' and objects' data.

The first word that differs decides, as its first byte that differs would:
both words byte-swapped (the processor is little-endian), the larger one is
the one whose first differing byte is larger. (no_builtin: the compiler
would otherwise make the byte loop a call to memcmp, this function.)
*/

#include <stdint.h>

/* (the compiler's size type: the builtin memcmp's, which the port's own
stddef.h may name otherwise, of the same width) */
__attribute__((no_builtin)) int memcmp(const void *left, const void *right, __SIZE_TYPE__ size)
{
	const unsigned char *l = left, *r = right;

	for (; size >= 8; size -= 8, l += 8, r += 8)
	{
		uint64_t a, b;

		__builtin_memcpy(&a, l, 8);
		__builtin_memcpy(&b, r, 8);
		if (a != b)
		{
			a = __builtin_bswap64(a);
			b = __builtin_bswap64(b);
			return a < b ? -1 : 1;
		}
	}
	for (; size; size--, l++, r++)
	{
		if (*l != *r)
			return *l < *r ? -1 : 1;
	}
	return 0;
}
