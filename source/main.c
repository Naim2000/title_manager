#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <ogc/es.h>
#include <gccore.h>
#include <fat.h>

#include "converter/converter.h"
#include "libpatcher/libpatcher.h"

#include "common.h"
#include "video.h"
#include "pad.h"
#include "menu.h"
#include "ncd.h"
#include "save.h"
#include "identify.h"
#include "wiimenu.h"

// snake case for snake year !!!

typedef struct title {
	union {
		uint64_t id;
		struct { uint32_t tid_hi, tid_lo; };
	};
	char      name[256];
	char      name_short[4];
	tmd_view* tmd_view;
	tikview*  ticket_views;
	uint32_t  num_tickets;
} title_t;

typedef struct title_category {
	uint32_t tid_hi;
	char     name[64];
	unsigned num_titles;
	title_t* title_list;
} title_category_t;

static title_category_t g_categories[] = {
	{
		.tid_hi = 0x00000001,
		.name   = "System titles"
	},

	{
		.tid_hi = 0x00010000,
		.name   = "Disc titles",
	},

	{
		.tid_hi = 0x00010001,
		.name   = "Downloadable channels",
	},

	{
		.tid_hi = 0x00010002,
		.name   = "System channels",
	},

	{
		.tid_hi = 0x00010004,
		.name   = "Disc titles with channels",
	},

	{
		.tid_hi = 0x00010005,
		.name   = "Downloadable game content",
	},

	{
		.tid_hi = 0x00010008,
		.name   = "Hidden titles",
	},
};
const unsigned g_num_categories = (sizeof(g_categories) / sizeof(title_category_t));

void free_title(title_t* title) {
	free(title->tmd_view);
	free(title->ticket_views);

	title->tmd_view = NULL;
	title->ticket_views = NULL;
}

void free_category(title_category_t* category) {
	for (title_t* title = category->title_list; title - category->title_list < category->num_titles; title++)
		free_title(title);

	free(category->title_list);
	category->title_list = NULL;
}

void free_all_categories(void) {
	for (int i = 0; i < g_num_categories; i++)
		free_category(&g_categories[i]);
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

int try_name_ios(title_t* title) {
	uint32_t slot     = title->tid_lo;
	uint16_t revision = title->tmd_view->title_version;
	int      ret, cfd;
	uint32_t data[0x10] __attribute__((aligned(0x20))) = {};

	if (slot == 254 && (revision == 31337 || revision == 0xFF01)) {
		strcpy(title->name, "BootMii IOS");
		return 2;
	}

	ret = cfd = ES_OpenTitleContent(title->id, title->ticket_views, 0);
	if (ret < 0) {
		print_error("ES_OpenTitleContent(%016llx)", ret, title->id);
		goto not_a_cios;
	}

	ret = ES_ReadContent(cfd, (u8 *)data, sizeof(data));
	ES_CloseContent(cfd);
	if (ret != sizeof(data)) { //?
		print_error("ES_ReadContent(%016llx)", ret, title->id);
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
	if ((revision & 0xFF) == 0x00 || revision == 404)
		strcpy(title->name + 8, "(Stub)");

	return 0;
}

int try_name_save_banner(title_t* title) {
	int                  ret, fd;
	char                 filepath[0x40] __attribute__((aligned(0x20))) = {};
	struct banner_header banner __attribute__((aligned(0x20)));

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

	ret = ISFS_Read(fd, &banner, sizeof(banner));
	ISFS_Close(fd);
	if (ret != sizeof(banner)) { // :v
		print_error("ISFS_Read(%016llx/banner.bin)", ret, title->id);
		return ret;
	}

	if (banner.magic != WIBN_MAGIC) {
		// ????
		fprintf(stderr, "%016llx has an invalid save banner?\n", title->id);
		return -1;
	}

	// Okay, do the thing.
	utf16_to_utf8(banner.game_title, 32, (utf8_t *)title->name, sizeof(title->name));
	return 0;
}

#define IMET_MAGIC 0x494D4554
typedef struct imet_header {
	uint8_t  padding[0x40];
	uint32_t magic; // IMET
	uint32_t data_size;
	uint32_t version;
	uint32_t file_sizes[3];
	uint32_t flags; //*
	utf16_t  names[10][2][21]; // language, main/alt (??), text (duh)
} imet_header;

int try_name_channel_banner(title_t* title) {
	int         ret, cfd;
	imet_header imet __attribute__((aligned(0x20)));
	char        temp[64];

	ret = cfd = ES_OpenTitleContent(title->id, title->ticket_views, 0);
	if (ret < 0) {
		print_error("ES_OpenTitleContent(%016llx)", ret, title->id);
		return ret;
	}

	ret = ES_SeekContent(cfd, 0x40, SEEK_SET);
	if (ret != 0x40) {
		print_error("ES_SeekContent(%016llx)", ret, title->id);
		ES_CloseContent(cfd);
		return ret;
	}

	ret = ES_ReadContent(cfd, (u8 *)&imet, sizeof(imet));
	ES_CloseContent(cfd);
	if (ret != sizeof(imet)) {
		print_error("ES_ReadContent(%016llx)", ret, title->id);
		return ret;
	}

	if (imet.magic != IMET_MAGIC) {
		fprintf(stderr, "%016llx has an invalid channel banner?\n", title->id);
		return -1;
	}

	utf16_to_utf8(imet.names[1][0], 21, (utf8_t *)title->name, sizeof(title->name));
	if (*imet.names[1][1]) {
		utf16_to_utf8(imet.names[1][1], 21, (utf8_t *)temp, sizeof(temp));
		sprintf(strrchr(title->name, 0), " (%s)", temp);
	}
	return 0;
}

static int cmp_title(const void* a_, const void* b_) {
	const title_t *a = a_, *b = b_;

	return a->tid_lo - b->tid_lo;
}

bool try_name_short(uint64_t tid, char out[4]) {
	bool ret = true;

	if ((tid >> 32) == 0x00000001) {
		memset(out, '-', 4);
		return false;
	} else for (int i = 0; i < 4; i++) {
		char c = ((const char *)&tid)[i+4];
		if (c > 0x20 && c < 0x7F) {
			out[i] = c;
		} else {
			out[i] = '.';
			ret = false;
		}
	}

	return ret;
}

void try_name_title(title_t* title) {
	int ret;

	switch (title->tid_hi) {
		case 0x00000001: { // System titles
			// uint16_t revision = title->tmd_view->title_version;
			switch (title->tid_lo) {
				case 0x00000000: strcpy(title->name, "(Superuser ticket.)"); break;
				case 0x00000001: strcpy(title->name, "(boot2? IOS doesn't let you install this as a normal title)"); break;
				case 0x00000002: wiimenu_name_version(title->tmd_view->title_version, title->name); break;
				case 0x00000100: strcpy(title->name, "BC"); break;
				case 0x00000101: strcpy(title->name, "MIOS"); break;
				case 0x00000200: strcpy(title->name, "BC-NAND"); break;
				case 0x00000201: strcpy(title->name, "BC-WFS"); break;
				default:
					if (title->tid_lo < 0x100)
						try_name_ios(title);
					else
						strcpy(title->name, "<unknown>");
			}
		} break;

		case 0x00010008: { // Hidden titles.
			switch (title->tid_lo) {
				case 0x48414B45:
				case 0x48414B50:
				case 0x48414B4A: sprintf(title->name, "End-user license agreement (%c)", (char)(title->tid_lo & 0xFF)); break;

				case 0x48414C45:
				case 0x48414C50:
				case 0x48414C4A: sprintf(title->name, "Region Select (%c)", (char)(title->tid_lo & 0xFF)); break;

				case 0x4843434A: strcpy(title->name, "Set Personal Data"); break;

				default:         strcpy(title->name, "<unknown>"); break;
			}
		} break;

		default:
			// Saves have bigger name fields
			if ((ret = try_name_save_banner(title)) == 0)
				break;

			if (title->tid_hi != 0x00010000 || title->tid_lo == 0x48415A41 /* <- Dumb outlier */) {
				if ((ret = try_name_channel_banner(title)) < 0)

				sprintf(title->name, "try_name_channel_banner() => %i", ret);
			} else {
				sprintf(title->name, "try_name_save_banner() => %i", ret);
			}

			break;
	}
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
		title_category_t* target = NULL;
		title_t             temp = { .id = titles_raw[i] };
		uint32_t   tmd_view_size = 0;

		for (int j = 0; j < g_num_categories; j++) {
			if (g_categories[j].tid_hi == temp.tid_hi) {
				target = &g_categories[j];
				break;
			}
		}

		if (!target) {
			fprintf(stderr, "Title %016llx has unknown type %#010x\n", temp.id, temp.tid_hi);
			continue;
		}

		ret = ES_GetTMDViewSize(temp.id, &tmd_view_size);
		if (ret < 0) {
			print_error("ES_GetTMDViewSize(%016llx)", ret, temp.id);
			// continue;
		} else {
			temp.tmd_view = memalign32(tmd_view_size);
			if (!temp.tmd_view) {
				print_error("memory allocation", 0);
				continue;
			}

			ret = ES_GetTMDView(temp.id, (u8 *)temp.tmd_view, tmd_view_size);
			if (ret < 0) {
				print_error("ES_GetTMDView(%016llx)", ret, temp.id);
				goto we_gotta_go_bald;
			}
		}

		ret = ES_GetNumTicketViews(temp.id, &temp.num_tickets);
		if (ret < 0) {
			print_error("ES_GetNumTicketViews(%016llx)", ret, temp.id);
			goto we_gotta_go_bald;
		}

		if (temp.num_tickets) {
			temp.ticket_views = memalign32(sizeof(tikview) * temp.num_tickets);
			if (ret < 0) {
				print_error("memory allocation", 0);
				goto we_gotta_go_bald;
			}

			ret = ES_GetTicketViews(temp.id, temp.ticket_views, temp.num_tickets);
			if (ret < 0) {
				print_error("ES_GetTicketViews", ret);
				goto we_gotta_go_bald;
			}
		}

		if (temp.tmd_view) {
			try_name_title(&temp);
		} else {
			sprintf(temp.name, "<No title metadata?>");
		}

		try_name_short(temp.id, temp.name_short);

		title_t* tmp_list = reallocarray(target->title_list, target->num_titles + 1, sizeof(title_t));
		if (!tmp_list) {
			print_error("memory allocation", 0);
			break;
		}

		target->title_list = tmp_list;
		target->title_list[target->num_titles++] = temp;
		continue;

we_gotta_go_bald:
		free(temp.tmd_view);
		free(temp.ticket_views);
		continue;
	}

	// Lastly, let's do a bit of sorting
	for (int i = 0; i < g_num_categories; i++) {
		title_category_t* cat = &g_categories[i];

		qsort(cat->title_list, cat->num_titles, sizeof(title_t), cmp_title);
	}

	free(titles_raw);
	return 0;
}

title_t* find_title(uint64_t title_id) {
	uint32_t tid_hi = (uint32_t)(title_id >> 32);

	for (title_category_t* cat = g_categories; cat - g_categories < g_num_categories; cat++) {
		if (cat->tid_hi == tid_hi) {
			for (title_t* title = cat->title_list; title - cat->title_list < cat->num_titles; title++) {
				if (title->id == title_id)
					return title;
			}

			break;
		}
	}

	return NULL;
}

int uninstall_title(const title_t* title) {
	const char* no_touchy_reason = NULL;

	if (!title->tmd_view)
		goto clear;

	if (title->tid_hi == 0x00000001) {
		if (title->tid_lo == 0x00000002) {
			// ?
			exit(-1);
		}

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

		title_t* wiimenu = find_title(0x0000000100000002);
		if (!wiimenu) {
			no_touchy_reason = "I can't find the Wii System Menu...?";
			goto no_touchy;
		}

		if (title->id == wiimenu->tmd_view->sys_version) {
			no_touchy_reason = "The Wii System Menu runs on this IOS!!!!";
			goto no_touchy;
		}

		if (title->tid_lo != 0x00000000 && title->tid_lo < 200) {
			no_touchy_reason = "I don't trust you with uninstalling normal IOS.";
			goto no_touchy;
		}
	}
	else if (title->tid_hi == 0x00010008) {
		uint32_t tid_superlow = title->tid_lo & ~0xFF;
		if (tid_superlow != 0x48414B00 && tid_superlow != 0x48414C00) {
			goto clear;
		}

		title_t* wiimenu = find_title(0x0000000100000002);
		if (!wiimenu) {
			no_touchy_reason = "I can't find the Wii System Menu...?";
			goto no_touchy;
		}

		if (wiimenu_version_is_official(wiimenu->tmd_view->title_version)) {
			char region = wiimenu_region_table[1][wiimenu->tmd_view->title_version & 0x1F];
			if (title->tid_lo == (tid_superlow ^ region)) {
				goto no_touchy;
			}
		} else {
			no_touchy_reason = "I can't determine the Wii System Menu's region!";
			goto no_touchy;
		}
	} else if (title->tid_hi == 0x00010001) {
		if (title->tid_lo == 0x48415858 ||
			title->tid_lo == 0x4A4F4449 ||
			title->tid_lo == 0xAF1BF516 ||
			title->tid_lo == 0x4C554C5A ||
			title->tid_lo == 0x4F484243)
		{
			no_touchy_reason = "Uhh...... use Data management?\nWhat are you trying to do here?";
			goto no_touchy;
		}
	}

clear:
	int ret;
	static tikview tikview_buffer __attribute__((aligned(0x20)));

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
/*
	free(title->ticket_views);
	title->ticket_views = NULL;
	title->num_tickets  = 0;

	free(title->tmd_view);
	title->tmd_view = NULL;

	strcpy(title->name, "<Uninstalled.>");
*/
	return 0;

no_touchy:
	printf("Please do not uninstall \"%s\"!\n", title->name);
	if (no_touchy_reason)
		printf("I am keeping him alive because:\n	\"%s\"\n", no_touchy_reason);

	return -1017;
}

int dump_title_save(const title_t* title) {
	int   ret;
	FILE* fp = NULL;
	char  file_name[32];

	if (!(
		title->tid_hi == 0x00010000 ||
		title->tid_hi == 0x00010001 ||
		title->tid_hi == 0x00010004
	)) {
		puts("Does this even have a proper save?");
		return -1;
	}

	if (!try_name_short(title->id, file_name)) {
		puts("try_name_short() said no");
		return -2;
	}

	strcpy(file_name + 4, "-data.bin");
	fp = fopen(file_name, "wb");
	if (!fp) {
		perror(file_name);
		return -3;
	}

	ret = export_save(title->id, fp);
	fclose(fp);
	if (ret < 0) {
		// print_error("export_save", ret);
		remove(file_name);
	}

	return ret;
}

int extract_title_save(const title_t* title) {
	int  ret;
	char dir_path[64];

	if (!try_name_short(title->id, dir_path))
		sprintf(dir_path, "%016llx", title->id);

	ret = mkdir(dir_path, 0644);
	if (ret < 0 && errno != EEXIST) {
		perror(dir_path);
		return -errno;
	}

	ret = extract_save(title->id, dir_path);

	return ret;
}

void print_title_header(const void* p) {
	const title_t* title = p;

	print_this_dumb_header();
	printf("Name:        %s\n", title->name);
	printf("Title ID:    %08x-%08x (%.4s)\n", title->tid_hi, title->tid_lo, title->name_short);
	// printf("Region:      %#04hhx\n", title->tmd_view->);
	if (title->tmd_view) {
		printf("Revision:    v%hu (%#hx)\n", title->tmd_view->title_version, title->tmd_view->title_version);
		if (title->tmd_view->sys_version >> 32 == 0x00000001)
			printf("IOS version: IOS%u\n", (uint32_t)title->tmd_view->sys_version);
	}

	print_this_dumb_line();
}

void manage_title_menu(const void* p) {
	const title_t* title = p;

	int                  cursor = 0;
	const int       num_options = 4;
	const char* const options[] = { "Uninstall this title",
	                                "Dump save data (data.bin)",
	                                "Dump save data (extract)",
	                                "Dump title (.wad) (X)" };

	while (true) {
		print_title_header(title);

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
						puts("Press +/START to confirm. \nPress any other button to cancel.");
						sleep(2);
						if (wait_button(0) & WPAD_BUTTON_PLUS)
							uninstall_title(title);

					} break;

					case 1: {
						dump_title_save(title);
					} break;

					case 2: {
						extract_title_save(title);
					} break;

					default: {
						puts("Unimplemented. Sorry.");
					} break;
				}


				puts("\nPress any button to continue...");
				wait_button(0);
				return;
			} break;

			case WPAD_BUTTON_B:
			case WPAD_BUTTON_HOME: {
				return;
			} break;
		}
	}
}

const char* name_title(const void* p, char buffer[256]) {
	const title_t* title = p;

	if (title->tid_hi == 0x00000001)
		snprintf(buffer, 256, "[%08x] - %.128s", title->tid_lo, title->name);
	else
		snprintf(buffer, 256, "[%08x] (%.4s) - %.128s", title->tid_lo, title->name_short, title->name);

	return buffer;
}

void print_category_header(const void* p, int cursor, int count) {
	const title_category_t* cat = p;

	print_this_dumb_header();
	printf("Selected Category - [%08x] %s :: Item %u out of %u\n\n", cat->tid_hi, cat->name, cursor + 1, count);
}

void manage_category_menu(const void* p) {
	const title_category_t* cat = p;

	menu_item_list_t title_list = {
		.print_header = print_category_header,
		.header_ptr   = cat,
		.items        = cat->title_list,
		.item_size    = sizeof(title_t),
		.num_items    = cat->num_titles,
		.max_items    = conY - 8,
		.get_name     = name_title,
		.select       = manage_title_menu,
	};

	if (cat->num_titles)
		ItemMenu(&title_list);
}

const char* name_category(const void* p, char buffer[256]) {
	const title_category_t* cat = p;

	snprintf(buffer, 256, "[%08x] %s (%u)", cat->tid_hi, cat->name, cat->num_titles);
	return buffer;
}

extern void __exception_setreload(int seconds);

int main(int argc, char* argv[]) {
	int ret;

	// __exception_setreload(5);

	puts("Loading..."); // Wii mod lite reference !!!
	if (!apply_patches()) {
		sleep(5);
		return -1;
	}

	initpads();
	SHA_Init();
	NCD_Init();
	ret = ISFS_Initialize();
	if (ret < 0) {
		print_error("ISFS_Initialize", ret);
		return ret;
	}

	fatInitDefault();

	identify_sm();
	populate_title_categories();

	menu_item_list_t category_list = {
		.items        = g_categories,
		.item_size    = sizeof(title_category_t),
		.num_items    = g_num_categories,
		.get_name     = name_category,
		.select       = manage_category_menu,
	};

	// wait_button(0);
	ItemMenu(&category_list);

	free_all_categories();
	stoppads();
	NCD_Shutdown();
	SHA_Close();
	ISFS_Deinitialize();
	return 0;
}
