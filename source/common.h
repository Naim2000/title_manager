#include <stdio.h>
#include <stdlib.h>

#define align_up(x, align) __builtin_align_up(x, align)

#define print_error(func, ret, ...) do { fprintf(stderr, "%s:%i : " func " failed (ret=%i)\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__, ret); } while (0);

static inline void* memalign32(size_t size) {
	return aligned_alloc(0x20, align_up(size, 0x20));
}
