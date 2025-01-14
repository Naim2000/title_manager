#include <stdio.h>
#include <stdint.h>

#include "menu.h"
#include "video.h"
#include "pad.h"

void print_this_dumb_line(void) {
	static char line[100];
	if (!*line) memset(line, 0xc4, sizeof(line));

	printf("%.*s", conX, line);
}

void print_this_dumb_header(void) {
	clear();
	puts("title_manager_by_thepikachugamer");
	puts("happy snake year 2025!!!!!");
	print_this_dumb_line();
}

void ItemMenu(menu_item_list_t* list) {
	int cursor = 0;
	int start  = 0;
	int max    = list->max_items ?: conY - 5;
	int count  = list->num_items;

	while(true) {
		char buffer[256];

		if (list->print_header)
			list->print_header(list->header_ptr, cursor, count);
		else
			print_this_dumb_header();

		for (int i = start; i - start < max; i++) {
			if (i >= count) {
				putchar('\n');
				continue;
			}

			const void* item = list->items + (list->item_size * i);
			const char* name = list->get_name(item, buffer);

			printf("%3s %.*s\n", (cursor == i) ? " >>" : "", conX - 5, name);
		}

		switch (wait_button(WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_HOME)) {
			case WPAD_BUTTON_DOWN: {
				if (cursor >= (count - 1))
					start = cursor = 0;

				else if ((++cursor - start) >= max)
					start++;
			} break;

			case WPAD_BUTTON_UP: {
				if (cursor <= 0) {
					cursor = count - 1;
					if (cursor >= max)
						start = 1 + cursor - max;
				}

				else if (--cursor < start)
					start--;

			} break;

			case WPAD_BUTTON_RIGHT: {
				cursor = count - 1;

				if (cursor >= max)
					start = 1 + cursor - max;
			} break;

			case WPAD_BUTTON_LEFT: {
				cursor = start = 0;
			} break;

			case WPAD_BUTTON_A: {
				list->select(list->items + (list->item_size * cursor));
			} break;

			case WPAD_BUTTON_B:
			case WPAD_BUTTON_HOME: {
				return;
			} break;
		}
	}
}
