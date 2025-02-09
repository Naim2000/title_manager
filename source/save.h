#include <stdint.h>
#include <assert.h>
#include <ogc/es.h>
#include <mbedtls/md5.h>
#include "converter/converter.h"

#define CHECK_STRUCT_SIZE(T, size) _Static_assert(sizeof(T) == (size), "Size of " #T " is not " #size)

static const uint32_t sd_initial_iv[4] = { 0x216712e6, 0xaa1f689f, 0x95c5a223, 0x24dc6a98 };
static const uint32_t   md5_blanker[4] = { 0x0e653781, 0x99be4517, 0xab06ec22, 0x451a5793 };

struct ecc_cert {
	sig_ecdsa  signature;
	cert_ecdsa certificate;
};
CHECK_STRUCT_SIZE(struct ecc_cert, 0x180);

// gonna need this one from you dolphin-emu
enum {
	FULL_BNR_MIN = 0x72a0,  // BNR_SZ + 1*ICON_SZ
	FULL_BNR_MAX = 0xF0A0,  // BNR_SZ + 8*ICON_SZ
	BK_LISTED_SZ = 0x70,    // Size before rounding to nearest block
	SIG_SZ = 0x40,
	ECC_CERT_SZ = sizeof(struct ecc_cert),
	FULL_CERT_SZ = SIG_SZ + (2 * ECC_CERT_SZ) + 0x80, // ??

	BK_HDR_MAGIC = 0x426B0001,
	FILE_HDR_MAGIC = 0x03ADF17E,
	WIBN_MAGIC = 0x5749424E,
};

#define IMET_MAGIC 0x494D4554
typedef struct imet_header {
	uint8_t  padding[0x40];
	uint32_t magic; // IMET
	uint32_t header_size;
	uint32_t version;
	uint32_t file_sizes[3];
	uint32_t flags; //*
	utf16_t  names[10][2][21]; // language, main/alt (??), text (duh)
	uint8_t  padding2[0x24c];
	uint32_t md5_sum[4];
} imet_header;
CHECK_STRUCT_SIZE(imet_header, 0x600);

typedef struct save_header {
	uint64_t title_id;
	uint32_t banner_sz;
	uint8_t  permissions;
	uint8_t  attributes;
	uint32_t md5_sum[4];
	uint16_t unk2;
} __attribute__((packed)) save_header;
CHECK_STRUCT_SIZE(struct save_header, 0x20);

typedef struct content_header {
	uint64_t    title_id;
	uint32_t    icon_sz;
	uint32_t    header_md5[4];
	uint32_t    icon_md5[4];
	uint32_t    padding[5];
	imet_header imet;
} content_header;
CHECK_STRUCT_SIZE(struct content_header, 0x640);

typedef struct banner_header {
	uint32_t magic;
	uint32_t flags;
	uint16_t anim_speed;
	uint8_t  reserved[0x16];
	utf16_t  game_title[0x20];
	utf16_t  game_subtitle[0x20];
} banner_header;
CHECK_STRUCT_SIZE(struct banner_header, 0xA0);

typedef struct save_banner {
	struct banner_header header;

	uint8_t banner[0x6000];
	uint8_t icons[0x1200][8];
} save_banner;
CHECK_STRUCT_SIZE(struct save_banner, FULL_BNR_MAX);

typedef struct file_header {
	uint32_t magic;		// 0x03ADF17E
	uint32_t size;
	uint8_t  permissions;
	uint8_t  attributes;
	uint8_t  type;		// 1: File, 2: Directory
	char     name[0x40];
	char     padding[5];
	uint32_t iv[4];
	uint8_t  unknown[0x20];
	uint8_t  data[];
} file_header;
CHECK_STRUCT_SIZE(struct file_header, 0x80);

typedef struct bk_header {
	uint32_t header_size;	// 0x70
	uint32_t magic;			// 'Bk', 0x0001
	uint32_t device_id;
	uint32_t num_files;
	uint32_t total_files_size;
	uint32_t tmd_size;
	uint32_t total_contents_size;
	uint32_t total_size;
	uint8_t  included_contents[0x40];
	uint64_t title_id;
	uint8_t  mac_address[6];
	uint8_t  padding[0x12];

	struct file_header files[];
} bk_header;
CHECK_STRUCT_SIZE(struct bk_header, 0x80);

typedef struct data_bin {
	struct save_header header;
	struct save_banner banner;
} data_header;

struct file_entry {
	char     relative_path[64 - 31];
	uint8_t  permissions;
	uint8_t  attributes;
	uint8_t  type;
	uint32_t file_size;
};

struct file_table {
	unsigned int       num_entries;
	struct file_entry *entries;
};

int export_save(uint64_t title_id, FILE* out);
int extract_save(uint64_t title_id, const char* out_dir);
int export_content(uint64_t title_id, FILE* fp);
