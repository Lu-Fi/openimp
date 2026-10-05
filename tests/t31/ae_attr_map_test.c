/* T31 GetAeAttr/SetAeAttr: the 72-byte public struct goes through the
 * 0x98-byte driver block (src/isp/t31_ae_attr_map.h); a 0xA5 arena around
 * the public struct must stay intact and the map must be a round trip. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isp/t31_ae_attr_map.h"

int main(void)
{
	uint8_t arena[4096];
	uint32_t block[T31_AE_BLOCK_WORDS], pub[T31_AE_ATTR_WORDS];
	uint32_t *u = (uint32_t *)(void *)(arena + 256);
	int seen[T31_AE_BLOCK_WORDS] = { 0 };
	unsigned int i;

	assert(T31_AE_ATTR_WORDS * 4 == 72 && T31_AE_BLOCK_WORDS * 4 == 0x98);
	for (i = 0; i < T31_AE_ATTR_WORDS; i++) {
		assert(t31_ae_attr_map[i] < T31_AE_BLOCK_WORDS);
		assert(!seen[t31_ae_attr_map[i]]);   /* one-to-one */
		seen[t31_ae_attr_map[i]] = 1;
	}
	/* get: the driver fills the whole block, only 72 bytes reach the user */
	memset(arena, 0xa5, sizeof(arena));
	for (i = 0; i < T31_AE_BLOCK_WORDS; i++)
		block[i] = 0x1000 + i;
	for (i = 0; i < T31_AE_ATTR_WORDS; i++)
		u[i] = block[t31_ae_attr_map[i]];
	for (i = 0; i < sizeof(arena); i++)
		if (i < 256 || i >= 256 + 72)
			assert(arena[i] == 0xa5);
	assert(u[0] == 0x1000 && u[1] == 0x1000 + 13 && u[5] == 0x1000 + 36);
	/* set: back through the block gives the same public words */
	memcpy(pub, u, sizeof(pub));
	memset(block, 0, sizeof(block));
	for (i = 0; i < T31_AE_ATTR_WORDS; i++)
		block[t31_ae_attr_map[i]] = pub[i];
	for (i = 0; i < T31_AE_ATTR_WORDS; i++)
		assert(block[t31_ae_attr_map[i]] == u[i]);
	puts("t31 ae attr map: ok");
	return 0;
}
