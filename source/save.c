#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ogc/ipc.h>
#include <ogc/isfs.h>
#include <ogc/es.h>
#include <ogc/sha.h>
#include <mbedtls/aes.h>

#include "common.h"
#include "save.h"
#include "ncd.h"

#define roundup16(len) (((len) + 0x0F) &~ 0x0F)
#define roundup64(off) (((off) + 0x3F) &~ 0x3F)

static int get_bin_mode(const char* filepath, uint8_t* permissions, uint8_t* attributes) {
	uint32_t ownerID;
	uint16_t groupID;
	uint8_t
		pOwn, pGrp, pOth;

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

		if (!strcmp(file_name, "banner.bin")) {
			file_name += 11;
			continue;
		}

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

static int write_file_table(const char* data_path, struct file_table* file_table, struct file_header* file) {
	char     file_path[0x40] __attribute__((aligned(0x20)));
	int      ret, cnt = 0;

	strcpy(file_path, data_path);

	for (struct file_entry* entry = file_table->entries; entry - file_table->entries < file_table->num_entries; entry++) {
		__attribute__((aligned(0x20)))
		uint32_t iv[4] = { 0x74686570, 0x696b6163, 0x68756761, 0x6d65723c };

		file->magic       = FILE_HDR_MAGIC;
		strcpy(file->name,  entry->relative_path);
		file->type        = entry->type;
		file->size        = entry->file_size;
		file->permissions = entry->permissions;
		file->attributes  = entry->attributes;
		memcpy(file->iv,    iv, sizeof(iv));

		printf("Processing %s\n", entry->relative_path);

		if (file->type == 2) {
			file++;
			continue;
		}

		sprintf(file_path + 29, "/%s", entry->relative_path);
		int fd = ret = ISFS_Open(file_path, ISFS_OPEN_READ);
		if (ret < 0) {
			print_error("ISFS_Open(%s)", ret, file_path);
			return ret;
		}

		unsigned processed = 0;
		while (processed < file->size) {
			unsigned read = (file->size - processed > 0x10000) ? 0x10000 : file->size - processed;

			int ret = ISFS_Read(fd, file->data, read);
			if (ret != file->size) {
				print_error("ISFS_Read", ret);
				break;
			}

			ret = ES_Encrypt(ES_KEY_SDCARD, (u8*)iv, file->data, roundup64(read), file->data);
			if (ret < 0) {
				print_error("ES_Encrypt", ret);
				break;
			}

			processed += read;
		}

		ISFS_Close(fd);
		if (ret < 0)
			return ret;

		file = (struct file_header*)(file->data + roundup64(file->size));
	}

	return cnt;
}

int export_save(uint64_t title_id, FILE* fp) {
	int              ret;
	uint64_t         my_tid = -1;
	uint32_t         device_id = -1;
	char             data_path[ISFS_MAXPATH] __attribute__((aligned(0x20))) = {};
	struct data_bin  save __attribute__((aligned(0x40))) = {};
	struct bk_header temp = {};
	uint32_t         md5_sum[4];
	__attribute__((aligned(0x20)))
	uint32_t         _sd_iv[4];

	ret = ES_GetTitleID(&my_tid);
	if (ret < 0 || my_tid != 0x0000000100000002) {
		// Ok bet
		my_tid = 0x0000000100000002;
		ret = ES_GetStoredTMDSize(my_tid, &_sd_iv[2]);
		if (ret < 0) {
			print_error("ES_GetStoredTMDSize", ret);
			return ret;
		}

		signed_blob *s_tmd = memalign32(_sd_iv[2]);
		if (!s_tmd) {
			print_error("memory allocation", 0);
			return -1;
		}

		ret = ES_GetStoredTMD(my_tid, s_tmd, _sd_iv[2]);
		if (ret < 0) {
			print_error("ES_GetStoredTMD", ret);
			free(s_tmd);
			return ret;
		}

		int fd = ret = ISFS_Open("/ticket/00000001/00000002.tik", 1);
		if (ret < 0) {
			print_error("ISFS_Open", ret);
			free(s_tmd);
			return ret;
		}

		signed_blob *s_tik = memalign32(STD_SIGNED_TIK_SIZE);
		if (!s_tik) {
			print_error("memory allocation", 0);
			ISFS_Close(fd);
			free(s_tmd);
			return ret;
		}

		ret = ISFS_Read(fd, s_tik, STD_SIGNED_TIK_SIZE);
		ISFS_Close(fd);
		if (ret != STD_SIGNED_TIK_SIZE) {
			print_error("ISFS_Read", ret);
			free(s_tmd);
			free(s_tik);
			return ret;
		}

		fd = ret = ISFS_Open("/sys/cert.sys", 1);
		if (ret < 0) {
			print_error("ISFS_Open", ret);
			free(s_tmd);
			free(s_tik);
			return ret;
		}

		ret = ISFS_GetFileStats(fd, (fstats*)_sd_iv);
		if (ret < 0) {
			print_error("ISFS_GetFileStats", ret);
			ISFS_Close(fd);
			free(s_tmd);
			free(s_tik);
			return ret;
		}

		signed_blob *s_certs = memalign32(_sd_iv[0]);
		if (!s_certs) {
			print_error("memory allocation", 0);
			ISFS_Close(fd);
			free(s_tmd);
			free(s_tik);
		}

		ret = ISFS_Read(fd, s_certs, _sd_iv[0]);
		ISFS_Close(fd);
		if (ret != _sd_iv[0]) {
			print_error("ISFS_Read", ret);
			free(s_tmd);
			free(s_tik);
			free(s_certs);
			return ret;
		}

		// AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
		ret = ES_Identify(s_certs, _sd_iv[0], s_tmd, _sd_iv[2], s_tik, STD_SIGNED_TIK_SIZE, &_sd_iv[3]);
		if (ret < 0) {
			print_error("ES_Identify(%016llx)", ret, my_tid);
			return ret;
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

	int banner_sz = ret = ISFS_Read(fd, (void*)&save.banner, sizeof(save.banner));
	ISFS_Close(fd);
	if (ret < FULL_BNR_MIN) {
		print_error("ISFS_Read(banner.bin)", ret);
		return ret;
	}

	// nocopy
	save.banner.header.flags &= ~1;

	save.header.title_id  = title_id;
	save.header.banner_sz = banner_sz;
	ret = get_bin_mode(data_path, &save.header.permissions, &save.header.attributes);
	if (ret < 0) {
		// printf("Failed to get banner.bin permissions (%i)\n", ret);
		return ret;
	}

	memcpy(save.header.md5_sum, md5_blanker, sizeof(md5_sum));
	mbedtls_md5_ret(save.encrypted_data, sizeof(save.encrypted_data), (unsigned char*) md5_sum);
	memcpy(save.header.md5_sum, md5_sum, sizeof(md5_sum));

	memcpy(_sd_iv, sd_initial_iv, sizeof(sd_initial_iv));
	ES_Encrypt(ES_KEY_SDCARD, (u8*)_sd_iv, save.encrypted_data, sizeof(save.encrypted_data), save.encrypted_data);

	if (!fwrite(save.encrypted_data, sizeof(save.encrypted_data), 1, fp)) {
		print_error("fwrite", ret);
		return -errno;
	}

	*(strrchr(data_path, '/')) = 0;

	temp.magic       = BK_HDR_MAGIC;
	temp.header_size = BK_LISTED_SZ;
	temp.device_id   = device_id;
	NCD_Init();
	NCD_GetWirelessMacAddress(temp.mac_address);

	struct file_table table = {};
	ret = build_file_table(data_path, &table);
	if (ret < 0) {
		// print_error("build_file_table", ret);
		return ret;
	}

	temp.num_files = table.num_entries;
	for (struct file_entry* entry = table.entries; entry - table.entries < table.num_entries; entry++)
		temp.total_files_size += sizeof(struct file_header) + roundup64(entry->file_size);

	temp.total_size = temp.total_files_size + FULL_CERT_SZ;
	// Sigh
	size_t total_total_size = sizeof(struct bk_header) + temp.total_size;
	void* final_data = aligned_alloc(0x40, total_total_size);
	if (!final_data) {
		print_error("memory allocation", 0);
		return -1;
	}

	struct bk_header* bk_header = final_data;
	*bk_header = temp;
	ret = write_file_table(data_path, &table, bk_header->files);
	if (ret < 0) {
		// print_error("write_file_table", ret);
		return ret;
	}

	size_t sign_data_size = sizeof(struct bk_header) + bk_header->total_files_size;
	void  *p_apSignature  = final_data + sign_data_size;
	void  *p_deviceCert   = p_apSignature + SIG_SZ;
	void  *p_apCert       = p_deviceCert + ECC_CERT_SZ;

	ret = ES_GetDeviceCert(p_deviceCert);
	if (ret < 0) {
		print_error("ES_GetDeviceCert", ret);
		return ret;
	}

	ret = ES_Sign(final_data, sign_data_size, p_apSignature, p_apCert);
	if (ret < 0) {
		print_error("ES_Sign", ret);
		return ret;
	}

	if (!fwrite(final_data, total_total_size, 1, fp)) {
		print_error("fwrite", errno);
		return ret;
	}

	return 0;
}
