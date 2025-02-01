#include <stdint.h>
#include <stdbool.h>

extern const char wiimenu_region_table[2][7];

bool wiimenu_version_is_official(uint16_t);
void wiimenu_name_version(uint16_t, char* out);
