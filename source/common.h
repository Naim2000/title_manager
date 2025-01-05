#include <stdio.h>
#include <stdlib.h>

#define print_error(func, ret, ...) do { fprintf(stderr, "%s():%i : " func " failed (ret=%i)\n", __FUNCTION__, __LINE__, ##__VA_ARGS__, ret); } while (0);

static inline void* memalign32(size_t size) {
	return aligned_alloc(0x20, __builtin_align_up(size, 0x20));
}
