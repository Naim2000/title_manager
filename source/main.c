#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <ctype.h>
#include <ogc/es.h>
#include <gccore.h>

#include "converter/converter.h"
#include "libpatcher/libpatcher.h"

#include "video.h"
#include "pad.h"

// snake case for snake year !!!

struct title {
	union {
		uint64_t id;
		struct {
			uint32_t tid_hi, tid_lo;
		};
	};
	char      id_short[4];
	char      name[256];
	tmd_view* tmd_view;
	tikview*  ticket_views;
	uint32_t  num_tickets;
};

struct title_category {
	uint32_t      tid_hi;
	char          name[64];
	unsigned      num_titles;
	struct title* title_list;
};

static struct title_category g_categories[7] = {
	{
		.tid_hi = 0x00000001,
		.name   = "System titles (System menu/IOS/BC)"
	},

	{
		.tid_hi = 0x00010000,
		.name   = "Disc-based games",
	},

	{
		.tid_hi = 0x00010001,
		.name   = "Downloaded channels",
	},

	{
		.tid_hi = 0x00010002,
		.name   = "System channels",
	},

	{
		.tid_hi = 0x00010004,
		.name   = "Disc-based games with channels",
	},

	{
		.tid_hi = 0x00010005,
		.name   = "Downloadable game content",
	},

	{
		.tid_hi = 0x00010008,
		.name   = "Hidden titles"
	},
};

void free_title(struct title* title) {
	free(title->tmd_view);
	free(title->ticket_views);

	title->tmd_view = NULL;
	title->ticket_views = NULL;
}

void free_category(struct title_category* category) {
	for (struct title* title = category->title_list; title - category->title_list < category->num_titles; title++)
		free_title(title);

	free(category->title_list);
	category->title_list = NULL;
}

void free_all_categories(void) {
	for (int i = 0; i < 7; i++)
		free_category(&g_categories[i]);
}

#define print_error(func, ret, ...) do { fprintf(stderr, "%s():%i : " func " failed (ret=%i)\n", __FUNCTION__, __LINE__, ##__VA_ARGS__, ret); } while (0);

void* memalign32(size_t size) {
	return aligned_alloc(0x20, __builtin_align_up(size, 0x20));
}

bool get_content0(tmd_view* view, uint32_t* cid) {
	for (int i = 0; i < view->num_contents; i++) {
		tmd_view_content* p_content = &view->contents[i];

		if (p_content->index == 0) {
			*cid = p_content->cid;
			return true;
		}
	}

	return false;
}

int try_name_ios(struct title* title) {
	uint32_t slot     = title->tid_lo;
	uint16_t revision = title->tmd_view->title_version;
	uint32_t content  = 0;
	char     filepath[0x40] __attribute__((aligned(0x20)));
	int      ret, fd;
	uint32_t data[0x10] __attribute__((aligned(0x20))) = {};


	if (slot == 254 && (revision == 31337 || revision == 0xFF01)) {
		strcpy(title->name, "BootMii IOS");
		return 2;
	}

	get_content0(title->tmd_view, &content);

	sprintf(filepath, "/title/%08x/%08x/content/%08x.app", title->tid_hi, slot, content);
	ret = fd = ISFS_Open(filepath, 1);
	if (ret < 0) {
		print_error("ISFS_Open(%016llx/%08x.app)", ret, title->id, content);
		goto not_a_cios;
	}

	ret = ISFS_Read(fd, data, sizeof(data));
	ISFS_Close(fd);
	if (ret != sizeof(data)) { //?
		print_error("ISFS_Read(%016llx/%08x.app)", ret, title->id, content);
		goto not_a_cios;
	}

	//  Magic word               Header version
	if (data[0] == 0x1EE7C105 && data[1] == 0x00000001) {
		const char* cios_name = (const char*) &data[4];
		const char* cios_vers = (const char*) &data[8];

		//                                                                    cIOS version        Base IOS
		sprintf(title->name, "IOS %-3u (%s-v%u%s, base %u)", slot, cios_name, data[2], cios_vers, data[3]);
		return 1;
	}

not_a_cios:
	sprintf(title->name, "IOS %-3u (v%u.%u)", slot, (revision >> 8) & 0xFF, revision & 0xFF);
	if (revision == 0xFF00 || revision == 404)
		strcpy(title->name + 8, "(Stub)");

	return 0;
}

int try_name_save_banner(struct title* title) {
	int      ret, fd;
	char     filepath[0x40] __attribute__((aligned(0x20))) = {};
	uint32_t data[0x20] __attribute__((aligned(0x20)));

	ret = ES_GetDataDir(title->id, filepath);
	if (ret < 0) {
		print_error("ES_GetDataDir(%016llx)", ret, title->id);
		return ret;
	}

	strcat(filepath, "/banner.bin");
	ret = fd = ISFS_Open(filepath, 1);
	if (ret < 0) {
		if (ret != -106) print_error("ISFS_Open(%s)", ret, filepath);
		return ret;
	}

	ret = ISFS_Read(fd, data, sizeof(data));
	ISFS_Close(fd);
	if (ret != sizeof(data)) { // :v
		print_error("ISFS_Read(%016llx/banner.bin)", ret, title->id);
		return ret;
	}

	if (data[0] != 0x5749424E) { // WIBN
		// ????
		fprintf(stderr, "%016llx has an invalid banner?\n", title->id);
		return -1;
	}

	// Okay, do the thing.
	const utf16_t* save_name = (const utf16_t*) &data[8];
	utf16_to_utf8(save_name, 32, (utf8_t*)title->name, sizeof(title->name));
	return 0;
}

static int cmp_title(const void* a_, const void* b_) {
	const struct title *a = a_, *b = b_;

	return a->tid_lo - b->tid_lo;
}

int populate_title_categories(void) {
	int       ret;
	uint32_t  titles_cnt = 0;
	uint64_t* titles_raw = NULL;

	ret = ES_GetNumTitles(&titles_cnt);
	if (ret < 0) {
		print_error("ES_GetNumTitles", ret);
		return ret;
	}

	if (!titles_cnt) {
		puts("No titles installed?");
		return 0;
	}

	titles_raw = memalign32(titles_cnt * sizeof(uint64_t));
	if (!titles_raw) {
		print_error("memory allocation (%u titles)", 0, titles_cnt);
		return -1;
	}

	ret = ES_GetTitles(titles_raw, titles_cnt);
	if (ret < 0) {
		print_error("ES_GetTitles", ret);
		return ret;
	}

	for (int i = 0; i < titles_cnt; i++) {
		uint64_t tid                  = titles_raw[i]; // i[titles_raw] ???
		uint32_t tid_hi               = tid >> 32;
		uint32_t tid_lo               = tid & 0xFFFFFFFF;
		struct title_category* target = NULL;
		struct title             temp = {};
		uint32_t tmd_view_size        = 0;

		memset(&temp, 0, sizeof(temp));

		for (int j = 0; j < 7; j++) {
			if (g_categories[j].tid_hi == tid_hi) {
				target = &g_categories[j];
				break;
			}
		}

		if (!target) {
			fprintf(stderr, "Title %016llx has unknown type %#010x\n", tid, tid_hi);
			continue;
		}

		temp.id = tid;

		// 1: Get it's TMD view.
		ret = ES_GetTMDViewSize(tid, &tmd_view_size);
		if (ret < 0) {
			print_error("ES_GetTMDViewSize(%016llx)", ret, tid);
			continue;
		}

		temp.tmd_view = memalign32(tmd_view_size);
		if (!temp.tmd_view) {
			print_error("memory allocation (%016llx, %s, %u)", 0, tid, "tmd_view", tmd_view_size);
			continue;
		}

		ret = ES_GetTMDView(tid, (unsigned char*)temp.tmd_view, tmd_view_size);
		if (ret < 0) {
			print_error("ES_GetTMDView(%016llx)", ret, tid);
			goto we_gotta_go_bald;
		}

		// 2: Get it's ticket view(s) now.
		ret = ES_GetNumTicketViews(tid, &temp.num_tickets);
		if (ret < 0) {
			print_error("ES_GetNumTicketViews(%016llx)", ret, tid);
			goto we_gotta_go_bald;
		}

		if (temp.num_tickets) {
			temp.ticket_views = memalign32(sizeof(tikview) * temp.num_tickets);
			if (ret < 0) {
				print_error("memory allocation (%016llx, %s, %u * %u)", 0, tid, "ticket_views", temp.num_tickets, sizeof(tikview));
				goto we_gotta_go_bald;
			}

			ret = ES_GetTicketViews(tid, temp.ticket_views, temp.num_tickets);
			if (ret < 0) {
				print_error("ES_GetTicketViews(%016llx, %u)", ret, tid, temp.num_tickets);
				goto we_gotta_go_bald;
			}
		}

		// 3: Get a name for it.
		switch (tid_hi) {
			case 0x00000001: { // System titles
				// uint16_t revision = temp.tmd_view->title_version;
				switch (tid_lo) {
					case 0x00000000: strcpy(temp.name, "<Superuser ticket>"); break;
					case 0x00000001: strcpy(temp.name, "<boot2 (not really)>"); break;
					case 0x00000002: strcpy(temp.name, "Wii System Menu"); break; // TODO: Print the system menu version here cause we can do that
					case 0x00000100: strcpy(temp.name, "BC"); break;
					case 0x00000101: strcpy(temp.name, "MIOS"); break;
					case 0x00000200: strcpy(temp.name, "BC-NAND"); break;
					case 0x00000201: strcpy(temp.name, "BC-WFS"); break;
					default:
						if (tid_lo < 0x100)
							try_name_ios(&temp);
						else
							strcpy(temp.name, "<unknown>");
				}
			} break;

			case 0x00010008: { // Hidden titles.
				switch (tid_lo) {
					case 0x48414B45:
					case 0x48414B50:
					case 0x48414B4A: sprintf(temp.name, "End-user license agreement (%c)", (char)(tid_lo & 0xFF)); break;

					case 0x48414C45:
					case 0x48414C50:
					case 0x48414C4A: sprintf(temp.name, "Region Select (%c)", (char)(tid_lo & 0xFF)); break;

					case 0x4843434A: strcpy(temp.name, "Set Personal Data"); break;

					default: {
						if (try_name_save_banner(&temp) < 0)
							strcpy(temp.name, "<unknown>");
					} break;
				}
			} break;

			default: {
				// TODO: Parse the channel's banner here.
				if (try_name_save_banner(&temp) < 0)
					sprintf(temp.name, "parse %016llx's banner here", tid);
			} break;
		}

		// 4: Get a short name for it.
		if (tid_hi == 0x00000001) {
			memset(temp.id_short, '-', 4);
		} else for (int j = 0; j < 4; j++) {
			char c = ((const char*)&tid_lo)[j];
			temp.id_short[j] = (c > 0x20 && c < 0x7F) ? c : '.';
		}


		// 5: Actually put it in the category. Have a good pat on the back, %016llx.
/*
		printf("Title %016llx (%.4s):\n", tid, temp.id_short);
		printf("+ Name: %s\n", temp.name);
		printf("+ Ticket count: %u\n", temp.num_tickets);
		printf("+ Revision: %#06x\n", temp.tmd_view->title_version);
*/
		struct title* tmp_list = reallocarray(target->title_list, target->num_titles + 1, sizeof(struct title));
		if (!tmp_list) {
			print_error("memory allocation (%u * %u)", 0, target->num_titles + 1, sizeof(struct title));
			break;
		}

		tmp_list[target->num_titles++] = temp;
		target->title_list = tmp_list;
		continue;

we_gotta_go_bald:
		free(temp.tmd_view);
		free(temp.ticket_views);
		continue;
	}

	// Lastly, let's do a bit of sorting
	for (int i = 0; i < 7; i++) {
		struct title_category* cat = &g_categories[i];

		qsort(cat->title_list, cat->num_titles, sizeof(struct title), cmp_title);
	}

	return 0;
}

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

struct title* find_title(uint64_t title_id) {
	uint32_t tid_hi = (uint32_t)(title_id >> 32);

	for (struct title_category* cat = g_categories; cat - g_categories < 7; cat++) {
		if (cat->tid_hi == tid_hi) {
			for (struct title* title = cat->title_list; title - cat->title_list < cat->num_titles; title++) {
				if (title->id == title_id)
					return title;
			}

			break;
		}
	}

	return NULL;
}

int uninstall_title(struct title* title) {
	const char* no_touchy_reason = NULL;

	if (title->tid_hi == 0x00000001) {
		if (title->tid_lo == 0x00000001 ||
		    title->tid_lo == 0x00000002 ||
		    title->tid_lo == 0x00000100 ||
		    title->tid_lo == 0x00000101 ||
		    title->tid_lo == 0x00000102 ||
		    title->tid_lo == 0x00000200 ||
		    title->tid_lo == 0x00000201)
		{
			no_touchy_reason = "That's a system title.";
			goto no_touchy;
		}

		if (title->tid_lo == 254 && title->tmd_view->title_version != 0xFF00) {
			goto no_touchy;
		}

		struct title* wii_system_menu = find_title(0x0000000100000002);
		if (!wii_system_menu) {
			no_touchy_reason = "I can't find the Wii System Menu...?";
			goto no_touchy;
		}

		if (title->id == wii_system_menu->tmd_view->sys_version) {
			no_touchy_reason = "The Wii System Menu runs on this IOS!!!!";
			goto no_touchy;
		}

		if (title->tid_lo != 0x00000000 && title->tid_lo < 200) {
			no_touchy_reason = "I don't trust you with uninstalling normal IOS.";
			goto no_touchy;
		}
	}

	int ret;
	tikview tikview_buffer __attribute__((aligned(0x20)));

	puts("Removing contents...");
	ret = ES_DeleteTitleContent(title->id);
	if (ret < 0)
		print_error("ES_DeleteTitleContents", ret);

	puts("Removing title...");
	ret = ES_DeleteTitle(title->id);
	if (ret < 0)
		print_error("ES_DeleteTitle", ret);

	puts("Removing ticket(s)");
	for (int i = 0; i < title->num_tickets; i++) {
		tikview_buffer = title->ticket_views[i];

		ret = ES_DeleteTicket(&tikview_buffer);
		if (ret < 0)
			print_error("ES_DeleteTicket(%i:%#018llx)", ret, i, tikview_buffer.ticketid);
	}

	puts("OK!");
	return 0;

no_touchy:
	printf("Please do not uninstall \"%s\"!\n", title->name);
	if (no_touchy_reason)
		printf("I am keeping him alive because:\n	\"%s\"\n", no_touchy_reason);

	return -1017;
}

void manage_title_menu(struct title* title) {
	int                  cursor = 0;
	const int       num_options = 4;
	const char* const options[] = { "Uninstall this title",
	                                "Dump save data (data.bin) (X)",
	                                "Dump save data (extract) (X)",
	                                "Dump title (.wad) (X)" };

	while (true) {
		print_this_dumb_header();
		printf("Name:        %s\n", title->name);
		printf("Title ID:    %016llx (%.4s)\n", title->id, title->id_short);
		// printf("Region:      %#04hhx\n", title->tmd_view->);
		printf("Revision:    %u (%#06hx)\n", title->tmd_view->title_version, title->tmd_view->title_version);
		printf("sys_version: %016llx\n", title->tmd_view->sys_version);
		print_this_dumb_line();

		for (int i = 0; i < num_options; i++)
			printf("	%3s %s\n", (cursor == i) ? " >>" : "", options[i]);

		putchar('\n');

		switch (wait_button(WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_HOME)) {
			case WPAD_BUTTON_UP: {
				if (!cursor--)
					cursor = num_options - 1;
			} break;

			case WPAD_BUTTON_DOWN: {
				cursor = (cursor + 1) % num_options;
			} break;

			case WPAD_BUTTON_RIGHT: {
				cursor = num_options - 1;
			} break;

			case WPAD_BUTTON_LEFT: {
				cursor = 0;
			} break;

			case WPAD_BUTTON_A: {
				switch (cursor) {
					case 0: {
						uninstall_title(title);
					} break;
					default: {
						puts("Unimplemented. Sorry.");
					} break;
				}

				return;
			} break;

			case WPAD_BUTTON_B:
			case WPAD_BUTTON_HOME: {
				return;
			} break;
		}
	}
}

void manage_category_menu(struct title_category* cat) {
	int cursor = 0;
	int start  = 0;
	int count  = cat->num_titles;
	int max    = conY - 8;
	// int style  = 0;


	while (true) {
		print_this_dumb_header();
		printf("Selected category - %08x (%s)\n\n", cat->tid_hi, cat->name);

		if (!cat->num_titles) {
			puts("Nothing's here.");
			wait_button(0);
			return;
		}

		for (int i = start; i - start < max; i++) {
			struct title* title = &cat->title_list[i];

			if (i >= count) {
				putchar('\n');
				continue;
			}

			printf("%3s [%08x] (%.4s) - %.*s\n", (i == cursor) ? " >>" : "", title->tid_lo, title->id_short, conX - 25, title->name);
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
				cursor = 7 - 1;
			} break;

			case WPAD_BUTTON_LEFT: {
				cursor = 0;
			} break;

			case WPAD_BUTTON_A: {
				manage_title_menu(&cat->title_list[cursor]);
				puts("Press any button to continue...");
				wait_button(0);
			} break;

			case WPAD_BUTTON_B:
			case WPAD_BUTTON_HOME: {
				return;
			} break;


		}
	}
}

extern void __exception_setreload(int us);

int main(int argc, char* argv[]) {
	int ret;
	int cursor = 0;

	__exception_setreload(5);

	if (!apply_patches()) {
		sleep(5);
		return -1;;
	}

	initpads();

	ret = ISFS_Initialize();
	if (ret < 0) {
		print_error("ISFS_Initialize", ret);
		return ret;
	}

	populate_title_categories();

	while (true) {
		print_this_dumb_header();

		for (int i = 0; i < 7; i++) {
			struct title_category* cat = &g_categories[i];

			printf("%3s [%08x] %s (%u)\n", (i == cursor) ? " >>" : "", cat->tid_hi, cat->name, cat->num_titles);
		}

		switch (wait_button(WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_HOME)) {
			case WPAD_BUTTON_UP: {
				if (!cursor--)
					cursor = 7 - 1;
			} break;

			case WPAD_BUTTON_DOWN: {
				cursor = (cursor + 1) % 7;
			} break;

			case WPAD_BUTTON_RIGHT: {
				cursor = 7 - 1;
			} break;

			case WPAD_BUTTON_LEFT: {
				cursor = 0;
			} break;

			case WPAD_BUTTON_A: {
				manage_category_menu(&g_categories[cursor]);
			} break;

			// case WPAD_BUTTON_B:
			case WPAD_BUTTON_HOME: {
				free_all_categories();
				return 0;
			} break;
		}

	}

	free_all_categories();
	return 0;
}

// __attribute__((destructor))
void __reset_to_exit(void) {
	puts("Press RESET to exit.");
	while (! SYS_ResetButtonDown())
		;
}
