#include <stdint.h>
#include <ogc/isfs.h>
#include <ogc/es.h>

#include "common.h"

int identify_sm(void) {
    int      ret;
    uint64_t title_id;
    uint32_t tmd_size;
    fstats   file_status __attribute__((aligned(0x20)));

    ret = ES_GetTitleID(&title_id);
	if (ret < 0 || title_id != 0x0000000100000002) {
		// Ok bet
		title_id = 0x0000000100000002;
		ret = ES_GetStoredTMDSize(title_id, &tmd_size);
		if (ret < 0) {
			print_error("ES_GetStoredTMDSize", ret);
			return ret;
		}

		signed_blob *s_tmd = memalign32(tmd_size);
		if (!s_tmd) {
			print_error("memory allocation", 0);
			return -1;
		}

		ret = ES_GetStoredTMD(title_id, s_tmd, tmd_size);
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

		ret = ISFS_GetFileStats(fd, &file_status);
		if (ret < 0) {
			print_error("ISFS_GetFileStats", ret);
			ISFS_Close(fd);
			free(s_tmd);
			free(s_tik);
			return ret;
		}

		signed_blob *s_certs = memalign32(file_status.file_length);
		if (!s_certs) {
			print_error("memory allocation", 0);
			ISFS_Close(fd);
			free(s_tmd);
			free(s_tik);
		}

		ret = ISFS_Read(fd, s_certs, file_status.file_length);
		ISFS_Close(fd);
		if (ret != file_status.file_length) {
			print_error("ISFS_Read", ret);
			free(s_tmd);
			free(s_tik);
			free(s_certs);
			return ret;
		}

		// AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
		ret = ES_Identify(s_certs, file_status.file_length, s_tmd, tmd_size, s_tik, STD_SIGNED_TIK_SIZE, NULL);
		free(s_tmd);
		free(s_tik);
		free(s_certs);
		 if (ret < 0) {
			title_id = 0;
			print_error("ES_Identify(%016llx)", ret, title_id);
			return ret;
		}

		ES_GetTitleID(&title_id);
	}

	return (title_id == 0x0000000100000002) ? 0 : -1;
}
