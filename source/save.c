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
#include "u8.h"
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
	char             data_path[ISFS_MAXPATH] __attribute__((aligned(0x20))) = {};
	struct data_bin *save = (struct data_bin *)buffer; // just enough space is here, and i personally didn't like the idea of.... 0xF0C0.... 64 kilos on my stack
	struct bk_header bk_header = {};
	uint32_t         iv[4] __attribute__((aligned(0x20)));
	uint32_t         hash[5] __attribute__((aligned(0x20)));
	// you all can go here too
	unsigned char   *ap_signature = buffer;
	struct ecc_cert *certificates = (struct ecc_cert *)(buffer + SIG_SZ); // [NG, AP] // WHERE IS MS!!!

	if (identify_sm() == 0) {
		ret = ES_SetUID(title_id);
		if (ret < 0) {
			print_error("ES_SetUID", ret);
		}
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
	mbedtls_md5_ret(buffer, sizeof(struct data_bin), (unsigned char *)save->header.md5_sum);

	memcpy(iv, sd_initial_iv, sizeof(sd_initial_iv));
	ES_Encrypt(ES_KEY_SDCARD, (u8*)iv, buffer, sizeof(struct data_bin), buffer);

	if (!fwrite(buffer, sizeof(struct data_bin), 1, fp)) {
		print_error("fwrite", ret);
		return -errno;
	}

	*(strrchr(data_path, '/')) = 0;

	bk_header.header_size = BK_LISTED_SZ;
	bk_header.magic       = BK_HDR_MAGIC;
	ret = ES_GetDeviceID(&bk_header.device_id);
	NCD_GetWirelessMacAddress(bk_header.mac_address);

	if (ret < 0) {
		print_error("ES_GetDeviceID", ret);
		return ret;
	}
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
	mbedtls_sha1_update_ret(&sha, (const unsigned char *)&bk_header, sizeof(bk_header));

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
	ret = ES_Sign(hash, sizeof(hash), ap_signature, (signed_blob *)&certificates[1]);
	if (ret < 0) {
		print_error("ES_Sign", ret);
		// return ret;
	}

	if (!fwrite(buffer, FULL_CERT_SZ, 1, fp))
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

int export_content(uint64_t title_id, FILE* fp) {
	int             ret, cfd = -1, cfdx = -1;
	uint32_t        n_views = 0;
	tikview        *p_views = NULL;
	U8Context       ctx = {};
	U8File          meta_icon = {};
	content_header *header = (content_header *)buffer;
	U8Header       *u8_header = (U8Header *)(buffer + sizeof(*header));
	bk_header      *bk_header = (struct bk_header *)buffer;
	signed_blob    *s_tmd = NULL;
	void           *ptr_icon = NULL;
	uint32_t        iv[4];


	ret = ES_GetNumTicketViews(title_id, &n_views);
	if (ret < 0) {
		print_error("ES_GetNumTicketViews", ret);
		goto exit;
	}

	p_views = memalign32(sizeof(tikview) * n_views);
	if (!p_views) {
		print_error("memory allocation", 0);
		goto exit;
	}

	ret = ES_GetTicketViews(title_id, p_views, n_views);
	if (ret < 0) {
		free(p_views);
		print_error("ES_GetTicketViews", ret);
		goto exit;
	}

	ret = cfd = ES_OpenTitleContent(title_id, p_views, 0);
	free(p_views);
	if (ret < 0) {
		print_error("ES_OpenTitleContent", ret);
		goto exit;
	}

	ret = ES_ReadContent(cfd, buffer, sizeof(struct content_header));
	if (ret != sizeof(struct content_header)) {
		print_error("ES_ReadContent", ret);
		goto exit;
	}

	// memset(buffer, 0, 0x40); // build tag was here
	for (int i = 0; i < 0x40; i += 4)
		memcpy(buffer + i, ":3c", 4);

	ret = ES_ReadContent(cfd, (u8 *)u8_header, sizeof(U8Header));
	if (ret != sizeof(U8Header)) {
		print_error("ES_ReadContent", ret);
		goto exit;
	}

	if (u8_header->magic != U8_MAGIC) {
		fprintf(stderr, "What's up with this banner? (U8 header is invalid!)\n");
		ret = -1;
		goto exit;
	}

	unsigned full_hdr_size = u8_header->root_node_offset + u8_header->meta_size;
	ES_SeekContent(cfd, -sizeof(U8Header), SEEK_CUR);
	ret = ES_ReadContent(cfd, (u8 *)u8_header, full_hdr_size);
	if (ret != full_hdr_size) {
		print_error("ES_ReadContent", ret);
		goto exit;
	}

	ret = U8Init(u8_header, &ctx);
	if (ret != 0) {
		print_error("U8Init", ret);
		goto exit;
	}

	ret = U8OpenFile(&ctx, "/meta/icon.bin", &meta_icon);
	if (ret != 0) {
		print_error("U8OpenFile(%s)", ret, "/meta/icon.bin");
		goto exit;
	}

	unsigned icon_size64 = align_up(meta_icon.size, 0x40);
	ptr_icon = memalign32(icon_size64);
	if (!ptr_icon) {
		print_error("memory allocation", 0);
		goto exit;
	}
	memset(ptr_icon, 0, icon_size64);

	unsigned meta_offset = sizeof(*header) + meta_icon.offset;
	ret = ES_SeekContent(cfd, meta_offset, SEEK_SET);
	if (ret != meta_offset) {
		print_error("ES_SeekContent(%#x)", ret, meta_offset);
		goto exit;
	}

	ret = ES_ReadContent(cfd, ptr_icon, meta_icon.size);
	ES_CloseContent(cfd);
	cfd = -1;
	if (ret != meta_icon.size) {
		print_error("ES_ReadContent(%#x)", ret, meta_icon.size);
		goto exit;
	}
	mbedtls_md5_ret(ptr_icon, icon_size64, (unsigned char *)header->icon_md5);

	header->title_id = title_id;
	header->icon_sz  = meta_icon.size;
	memset(header->imet.md5_sum, 0, sizeof(header->imet.md5_sum));
	memcpy(header->header_md5, md5_blanker, sizeof(header->header_md5));
	mbedtls_md5_ret((const unsigned char *)header, sizeof(*header), (unsigned char *)header->header_md5);

	// we are clear now
	memcpy(iv, sd_initial_iv, sizeof(iv));
	ret = ES_Encrypt(ES_KEY_SDCARD, iv, header, sizeof(content_header), header);
	if (ret < 0) {
		print_error("ES_Encrypt", ret);
		goto exit;
	}

	memcpy(iv, sd_initial_iv, sizeof(iv));
	ret = ES_Encrypt(ES_KEY_SDCARD, iv, ptr_icon, icon_size64, ptr_icon);
	if (ret < 0) {
		print_error("ES_Encrypt", ret);
		goto exit;
	}

	if (!fwrite(header,   sizeof(*header), 1, fp)
	||	!fwrite(ptr_icon, icon_size64,     1, fp))
	{
		print_error("fwrite", errno);
		ret = -errno;
		goto exit;
	}

	memset(bk_header, 0, sizeof(struct bk_header));

	bk_header->header_size = BK_LISTED_SZ;
	bk_header->magic       = BK_HDR_MAGIC;
	bk_header->title_id    = title_id;
	NCD_GetWirelessMacAddress(bk_header->mac_address); // for fun

	ret = ES_GetDeviceID(&bk_header->device_id);
	if (ret < 0) {
		print_error("ES_GetDeviceID", ret);
		goto exit;
	}

	ret = ES_GetStoredTMDSize(title_id, &bk_header->tmd_size);
	if (ret < 0) {
		print_error("ES_GetStoredTMDSize", ret);
		goto exit;
	}

	s_tmd = memalign32(bk_header->tmd_size);
	if (!s_tmd) {
		print_error("memory allocation", 0);
		goto exit;
	}

	// got real
	ret = ES_ExportTitleInit(title_id, s_tmd, bk_header->tmd_size);
	if (ret < 0) {
		print_error("ES_ExportTitleInit", ret);
		goto exit;
	}

	tmd* p_tmd = SIGNATURE_PAYLOAD(s_tmd);
	// OK, let's work this out
	for (int i = 0; i < p_tmd->num_contents; i++) {
		tmd_content* con = &p_tmd->contents[i];

		if (con->type & 0x8000) // We don't care
			continue;

		bk_header->included_contents[con->index >> 3] |= 1 << (con->index & 0x7);
		bk_header->total_contents_size += align_up(con->size, 0x40);
	}

	unsigned tmd_size64 = align_up(bk_header->tmd_size, 0x40);
	bk_header->total_size = tmd_size64 + bk_header->total_contents_size + FULL_CERT_SZ; // ?

	// OK!
	mbedtls_sha1_context sha;
	mbedtls_sha1_starts_ret(&sha);
	mbedtls_sha1_update_ret(&sha, (const unsigned char *)bk_header, sizeof(struct bk_header));
	mbedtls_sha1_update_ret(&sha, (const unsigned char *)s_tmd, tmd_size64);

	if (!fwrite(bk_header, sizeof(struct bk_header), 1, fp)
	||	!fwrite(s_tmd, tmd_size64,  1, fp))
	{
		print_error("fwrite", errno);
		ret = -errno;
	}

	for (int i = 0; i < p_tmd->num_contents; i++) {
		tmd_content* con = &p_tmd->contents[i];

		if (con->type & 0x8000) // We don't care
			continue;

		ret = cfdx = ES_ExportContentBegin(title_id, con->cid);
		if (ret < 0) {
			print_error("ES_ExportContentBegin(%08x)", ret, con->cid);
			break;
		}

		unsigned processed = 0;
		while (processed < (unsigned)con->size) {
			unsigned process = (con->size - processed > sizeof(buffer)) ? sizeof(buffer) : con->size - processed;
			unsigned process64 = align_up(process, 0x40);

			ret = ES_ExportContentData(cfdx, buffer, process64);
			if (ret < 0) {
				print_error("ES_ExportContentData", ret);
				break;
			}

			mbedtls_sha1_update_ret(&sha, buffer, process64);

			if (!fwrite(buffer, process64, 1, fp)) {
				print_error("fwrite", -errno);
				ret = -errno;
				break;
			}

			processed += process64;
		}

		ES_ExportContentEnd(cfdx);
		if (ret < 0)
			break;
	}

	unsigned char   *ap_signature = buffer;
	struct ecc_cert *certificates = (struct ecc_cert *)(buffer + SIG_SZ);
	unsigned char   *hash         = (unsigned char *)&certificates[2];

	mbedtls_sha1_finish_ret(&sha, hash);

	ret = ES_GetDeviceCert((u8 *)&certificates[0]);
	if (ret < 0) {
		print_error("ES_GetDeviceCert", ret);
		goto exit;
	}

	ret = ES_Sign(hash, sizeof(sha1), ap_signature, (signed_blob *)&certificates[1]);
	if (ret < 0) {
		print_error("ES_Sign", ret);
		goto exit;
	}

	if (!fwrite(buffer, FULL_CERT_SZ, 1, fp)) {
		print_error("fwrite", -errno);
		ret = -errno;
	}

exit:
	free(ptr_icon);
	free(s_tmd);

	if (cfd >= 0)
		ES_CloseContent(cfd);

	ES_ExportTitleDone();

	return ret;
}
