#include <stdint.h>
#include <ogc/isfs.h>
#include <ogc/es.h>

#include "identify.h"
#include "common.h"
#include "libpatcher/libpatcher.h"

int identify_sm(void) {
	int      ret;
	uint64_t title_id;
	uint32_t tmd_size;
	tikview  ticket;

	if (!is_dolphin() && (ES_GetTitleID(&title_id) < 0 || title_id != 0x100000002LL)) {
		// Ok bet
		title_id = 0x100000002LL;
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

		ret = ES_GetTicketViews(title_id, &ticket, 1);
		if (ret < 0) {
			print_error("ES_GetTicketViews", ret);
			free(s_tmd); return ret;
		}

		ret = ES_DiVerifyWithTicketView(NULL, 0, s_tmd, tmd_size, &ticket, NULL);
		free(s_tmd);
		if (ret < 0) {
			title_id = 0;
			print_error("ES_DiVerifyWithTicketView(%016llx)", ret, title_id);
			return ret;
		}

		ES_GetTitleID(&title_id);
	}

	return (title_id == 0x100000002LL) - 1;
}
