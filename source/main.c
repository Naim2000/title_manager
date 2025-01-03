#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <ogc/es.h>
#include <gccore.h>

#include "video.h"
// #include "pad.h"
#include "converter/converter.h"

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
	unsigned      title_count;
	struct title* title_list;
};

static struct title_category categories[7] = {
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
}

void free_category(struct title_category* category) {
	for (struct title* title = category->title_list; title - category->title_list < category->title_count; title++)
		free_title(title);
}

void free_all_categories(void) {
	for (int i = 0; i < 7; i++)
		free_category(&categories[i]);
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
		goto not_a_cios; // No choice
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

		//                                                   Base IOS            cIOS Version
		sprintf(title->name, "IOS %3u[%u] <%s-v%u%s>", slot, data[3], cios_name, data[2], cios_vers);
		return 1;
	}

not_a_cios:
	sprintf(title->name, "IOS %3u (v%u.%u)", slot, (revision >> 8) & 0xFF, revision & 0xFF);
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
			if (categories[j].tid_hi == tid_hi) {
				target = &categories[j];
				break;
			}
		}

		if (!target) {
			printf("Title %016llx has unknown type %#010x\n", tid, tid_hi);
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
				print_error("memory allocation (%016llx, %s, %u * %u)", 0, tid, "ticket_view", temp.num_tickets, sizeof(tikview));
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
					case 0x00000000: strcpy(temp.name, "<Superuser ticket>");
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
						if (try_name_save_banner(&temp) != 0)
							strcpy(temp.name, "<unknown>");
					} break;
				}
			} break;

			default: {
				// TODO: Parse the channel's banner here.
				if (try_name_save_banner(&temp) != 0)
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
		struct title* tmp_list = reallocarray(target->title_list, target->title_count + 1, sizeof(struct title));
		if (!tmp_list) {
			print_error("memory allocation (%u * %u)", 0, target->title_count + 1, sizeof(struct title));
			break;
		}

		tmp_list[target->title_count++] = temp;
		target->title_list = tmp_list;
		continue;

we_gotta_go_bald:
		if (temp.tmd_view) free(temp.tmd_view);
		// free(temp.ticket_view);
		continue;
	}

	return 0;
}

int main(int argc, char* argv[]) {
	int ret;

	ret = ISFS_Initialize();
	if (ret < 0) {
		print_error("ISFS_Initialize", ret);
		return ret;
	}

	populate_title_categories();

	puts("Finished stats:");
	for (int i = 0; i < 7; i++) {
		struct title_category* category = &categories[i];

		printf("Category %#010x \"%s\": \n", category->tid_hi, category->name);
		printf("+ Count: %u\n", category->title_count);
	}

	printf("Total title count: %u\n", titles_cnt);

	free_all_categories();
	return 0;
}

__attribute__((destructor))
void __anykey_to_exit(void) {
	puts("Press RESET to exit.");
	while (! SYS_ResetButtonDown())
		;
}
