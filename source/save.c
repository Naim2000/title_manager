#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <ogc/ipc.h>
#include <ogc/isfs.h>
#include <ogc/es.h>
#include <ogc/sha.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

#include "common.h"
#include "save.h"
#include "ncd.h"
#include "identify.h"

__attribute__((aligned(0x40)))
static unsigned char buffer[0x10000];

static int get_bin_mode(const char* filepath, uint8_t* permissions, uint8_t* attributes) {
	uint32_t ownerID;
	uint16_t groupID;
	uint8_t  pOwn, pGrp, pOth;

	int ret = ISFS_GetAttr(filepath, &ownerID, &groupID, attributes, &pOwn, &pGrp, &pOth);
	if (ret < 0) {
		print_error("ISFS_GetAttr", ret);
		return ret;
	}

	*permissions =
		(pOwn & 3) << 4 |
		(pGrp & 3) << 2 |
		(pOth & 3) << 0;

	return 0;
}

static bool append_to_table(struct file_table* file_table, struct file_entry* file_entry) {
	struct file_entry* temp = reallocarray(file_table->entries, ++file_table->num_entries, sizeof(struct file_entry));
	if (!temp) {
		print_error("memory allocation", 0);
		return false;
	}

	file_table->entries = temp;
	file_table->entries[file_table->num_entries - 1] = *file_entry;
	return true;
}

static int build_file_table(const char* path, struct file_table* file_table) {
	int      ret;
	char     file_path[0x40];
	char*    file_list = NULL;
	uint32_t num_files = 0;

	strcpy(file_path, path);

	ret = ISFS_ReadDir(file_path, NULL, &num_files);
	if (ret < 0) {
		print_error("ISFS_ReadDir(%s)", ret, file_path);
		return ret;
	}

	char file_list_buf[12 + 1][num_files] __attribute__((aligned(0x20)));
	file_list = file_list_buf[0];
	ret = ISFS_ReadDir(file_path, file_list, &num_files);
	if (ret < 0) {
		print_error("ISFS_ReadDir(%s)", ret, file_path);
		return ret;
	}

	const char* file_name = file_list;
	for (int i = 0; i < num_files; i++) {
		uint32_t file_stats[2] __attribute__((aligned(0x20)));
		struct file_entry temp = {};

		sprintf(strrchr(file_path, 0), "/%s", file_name);
		strcpy(temp.relative_path, file_path + 30);

		ret = get_bin_mode(file_path, &temp.permissions, &temp.attributes);
		if (ret < 0)
			return ret;

		int fd = ret = ISFS_Open(file_path, 1);
		if (ret == ISFS_EINVAL) {
			temp.type = 2;
			temp.file_size = 0;
			if (!append_to_table(file_table, &temp)) {
				print_error("memory allocation", 0);
				return -1;
			}
			ret = build_file_table(file_path, file_table);
			if (ret < 0)
				return ret;
		}
		else if (ret < 0) {
			print_error("ISFS_Open(%s)", ret, file_path + 30);
			return ret;
		}
		else {
			temp.type = 1;
			ret = ISFS_GetFileStats(fd, (fstats*) file_stats);
			if (ret < 0) {
				print_error("ISFS_GetFileStats", ret);
				return ret;
			}

			temp.file_size = file_stats[0];
			if (!append_to_table(file_table, &temp)) {
				print_error("memory allocation", 0);
				return -1;
			}
		}

		file_name += strlen(file_name) + 1;
		*(strrchr(file_path, '/')) = '\0';
	}

	return 0;
}

static int write_file_table(const char* data_path, struct file_table* file_table, mbedtls_sha1_context* sha, FILE* fp) {
	char     file_path[0x40] __attribute__((aligned(0x20)));
	int      ret;

	strcpy(file_path, data_path);

	for (int i = 0; i < file_table->num_entries; i++) {
		struct file_entry* entry = &file_table->entries[i];

		__attribute__((aligned(0x20)))
		uint32_t iv[4] = { 0x74686570, 0x696b6163, 0x68756761, 0x6d65723c + i };
		__attribute__((aligned(0x40)))
		struct file_header file = {};

		file.magic       = FILE_HDR_MAGIC;
		strcpy(file.name,  entry->relative_path);
		file.type        = entry->type;
		file.size        = entry->file_size;
		file.permissions = entry->permissions;
		file.attributes  = entry->attributes;
		memcpy(file.iv,    iv, sizeof(iv));

		mbedtls_sha1_update_ret(sha, (const unsigned char *)&file, sizeof(file));
		if (!fwrite(&file, sizeof(file), 1, fp)) {
			print_error("fwrite", errno);
			return -errno;
		}

		if (file.type == 2 || strcmp(entry->relative_path, "banner.bin") == 0)
			continue;

		printf("Processing %s (%#x, %uKiB)\n", entry->relative_path, entry->file_size, (entry->file_size + 0x3FF) >> 10);
		sprintf(file_path + 29, "/%s", entry->relative_path);
		int fd = ret = ISFS_Open(file_path, ISFS_OPEN_READ);
		if (ret < 0) {
			print_error("ISFS_Open", ret);
			return ret;
		}

		unsigned processed = 0;
		while (processed < entry->file_size) {
			unsigned read = (entry->file_size - processed > sizeof(buffer)) ? sizeof(buffer) : entry->file_size - processed;

			int ret = ISFS_Read(fd, buffer, read);
			if (ret <= 0) {
				print_error("ISFS_Read", ret);
				break;
			}

			ret = ES_Encrypt(ES_KEY_SDCARD, (u8*)iv, buffer, align_up(read, 0x40), buffer);
			if (ret < 0) {
				print_error("ES_Encrypt", ret);
				break;
			}

			mbedtls_sha1_update_ret(sha, buffer, align_up(read, 0x40));

			if (!fwrite(buffer, align_up(read, 0x40), 1, fp)) {
				print_error("fwrite", errno);
				ret = -errno;
				break;
			}

			processed += read;
		}

		ISFS_Close(fd);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void free_file_table(struct file_table* table) {
	free(table->entries);
	table->entries = NULL;
	table->num_entries = 0;
}

int export_save(uint64_t title_id, FILE* fp) {
	int              ret;
	uint32_t         device_id = -1;
	char             data_path[ISFS_MAXPATH] __attribute__((aligned(0x20))) = {};
	struct data_bin *save = (struct data_bin *)buffer; // just enough space is here, and i personally didn't like the idea of.... 0xF0C0.... 64 kilos on my stack
	struct bk_header bk_header = {};
	uint32_t         iv[4] __attribute__((aligned(0x20)));
	uint32_t         hash[5] __attribute__((aligned(0x20)));
	// you all can go here too
	unsigned char   *ap_signature = buffer;
	struct ecc_cert *certificates = (struct ecc_cert *)(buffer + SIG_SZ); // [NG, AP]

	if (identify_sm() == 0) {
		ret = ES_SetUID(title_id);
		if (ret < 0) {
			print_error("ES_SetUID", ret);
		}
	}

	ret = ES_GetDeviceID(&device_id);
	if (ret < 0) {
		print_error("ES_GetDeviceID", ret);
		return ret;
	}

	ret = ES_GetDataDir(title_id, data_path);
	if (ret < 0) {
		print_error("ES_GetDataDir", ret);
		return ret;
	}

	strcat(data_path, "/banner.bin");
	int fd = ret = ISFS_Open(data_path, ISFS_OPEN_READ);
	if (ret < 0) {
		print_error("ISFS_Open(banner.bin)", ret);
		return ret;
	}

	int banner_sz = ret = ISFS_Read(fd, (void*)&save->banner, sizeof(save->banner));
	ISFS_Close(fd);
	if (ret < FULL_BNR_MIN) {
		print_error("ISFS_Read(banner.bin)", ret);
		return ret;
	}

	// nocopy
	save->banner.header.flags &= ~1;

	save->header.title_id  = title_id;
	save->header.banner_sz = banner_sz;
	ret = get_bin_mode(data_path, &save->header.permissions, &save->header.attributes);
	if (ret < 0)
		return ret;

	memcpy(save->header.md5_sum, md5_blanker, sizeof(save->header.md5_sum));
	mbedtls_md5_ret(save->encrypted_data, sizeof(struct data_bin), (unsigned char *)save->header.md5_sum);

	memcpy(iv, sd_initial_iv, sizeof(sd_initial_iv));
	ES_Encrypt(ES_KEY_SDCARD, (u8*)iv, save->encrypted_data, sizeof(struct data_bin), save->encrypted_data);

	if (!fwrite(save->encrypted_data, sizeof(struct data_bin), 1, fp)) {
		print_error("fwrite", ret);
		return -errno;
	}

	*(strrchr(data_path, '/')) = 0;

	bk_header.magic       = BK_HDR_MAGIC;
	bk_header.header_size = BK_LISTED_SZ;
	bk_header.device_id   = device_id;
	NCD_GetWirelessMacAddress(bk_header.mac_address);

	struct file_table table = {};
	ret = build_file_table(data_path, &table);
	if (ret < 0)
		return ret;

	bk_header.num_files = table.num_entries;
	for (struct file_entry* entry = table.entries; entry - table.entries < table.num_entries; entry++)
		bk_header.total_files_size += sizeof(struct file_header) + align_up(entry->file_size, 0x40);

	bk_header.total_size = bk_header.total_files_size + FULL_CERT_SZ;

	// OK, Bk header is all done
	mbedtls_sha1_context sha;
	mbedtls_sha1_starts_ret(&sha);
	mbedtls_sha1_update_ret(&sha, (const unsigned char *)&bk_header, sizeof(struct bk_header));

	if (!fwrite(&bk_header, sizeof(struct bk_header), 1, fp)) {
		print_error("fwrite", errno);
		ret = -errno;
		goto foiled;
	}

	ret = write_file_table(data_path, &table, &sha, fp);
	if (ret < 0)
		goto foiled;

	mbedtls_sha1_finish_ret(&sha, (unsigned char *)hash);

	ret = ES_GetDeviceCert((u8 *)&certificates[0]);
	if (ret < 0) {
		print_error("ES_GetDeviceCert", ret);
		goto foiled;
	}

	//    vv This function is dumb
	ret = ES_Sign((u8 *)hash, sizeof(hash), ap_signature, (u8 *)&certificates[1]);
	if (ret < 0) {
		print_error("ES_Sign", ret);
		// return ret;
	}

	if (!fwrite(ap_signature,  SIG_SZ,          1, fp) ||
		!fwrite(certificates,  ECC_CERT_SZ * 2, 1, fp) ||
		!fwrite(table.entries, 0x80,            1, fp)) // Filler cause idk what this 128 byte padding is for
	{
		print_error("fwrite", errno);
		ret = -errno;
		goto foiled;
	}

	free_file_table(&table);
	return 0;

foiled:
	free_file_table(&table);
	return ret;
}

int extract_save(uint64_t title_id, const char* out_dir) {
	int   ret, fd;
	FILE *fp = NULL;
	char  data_path[ISFS_MAXPATH] __attribute__((aligned(0x20)));
	char  file_path[256];
	char *file_path_in, *file_path_out = stpcpy(file_path, out_dir);

	ret = ES_GetDataDir(title_id, data_path);
	if (ret < 0) {
		print_error("ES_GetDataDir(%016llx)", ret, title_id);
		return ret;
	}

	file_path_in = strrchr(data_path, 0);

	struct file_table table = {};
	ret = build_file_table(data_path, &table);
	if (ret < 0)
		return ret;

	for (int i = 0; i < table.num_entries; i++) {
		struct file_entry* entry =  &table.entries[i];

		sprintf(file_path_in,  "/%s", entry->relative_path);
		sprintf(file_path_out, "/%s", entry->relative_path);

		if (entry->type == 2) {
			ret = mkdir(file_path, 0644);
			if (ret < 0 && errno != EEXIST) {
				perror(file_path_out);
				return ret;
			}

			continue;
		}

		printf("Processing %s (%#x, %uKiB)\n", entry->relative_path, entry->file_size, (entry->file_size + 0x3FF) >> 10);
		ret = fd = ISFS_Open(data_path, ISFS_OPEN_READ);
		if (ret < 0) {
			print_error("ISFS_Open", ret);
			return ret;
		}

		fp = fopen(file_path, "wb");
		if (!fp) {
			ISFS_Close(fd);
			perror(file_path_out);
			return -errno;
		}

		unsigned processed = 0;
		while (processed < entry->file_size) {
			unsigned read = (entry->file_size - processed > sizeof(buffer)) ? sizeof(buffer) : entry->file_size - processed;

			int ret = ISFS_Read(fd, buffer, read);
			if (ret <= 0) {
				print_error("ISFS_Read", ret);
				break;
			}

			if (!fwrite(buffer, ret, 1, fp)) {
				print_error("fwrite", errno);
				ret = -errno;
				break;
			}

			processed += ret;
		}

		ISFS_Close(fd);
		fclose(fp);
		if (ret < 0)
			return ret;
	}

	return 0;
}
