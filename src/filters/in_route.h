	/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018-2024
 *					All rights reserved
 *
 *  This file is part of GPAC / ROUTE (ATSC3, DVB-I) input filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/route.h>
#include <gpac/network.h>
#include <gpac/download.h>
#include <gpac/thread.h>

#ifndef IN_ROUTE_H
#define IN_ROUTE_H

#ifndef GPAC_DISABLE_ROUTE

enum
{
	TSIO_FILE_PROGRESS = 1,
	TSIO_REPAIR_SCHEDULED = (1<<1)
};

typedef struct
{
	u32 sid;
	u32 tsi;
	GF_FilterPid *opid;
	//TOI of file being received - moved back to 0 once file is done being dispatched
	u32 current_toi;
	u32 bytes_sent;
	char *dash_rep_id;
	GF_List *pending_repairs;
	u32 flags_progress;
	Bool delete_first;
} TSI_Output;

typedef struct
{
	GF_FilterPid *opid;
	char *seg_name;
} SegInfo;

GF_OPT_ENUM (ROUTEInRepairMode,
	ROUTEIN_REPAIR_NO = 0,
	ROUTEIN_REPAIR_SIMPLE,
	ROUTEIN_REPAIR_STRICT,
	ROUTEIN_REPAIR_FULL,
);

typedef struct _route_repair_seg_info RepairSegmentInfo;

typedef struct
{
	u32 br_start;
	u32 br_end;
	u32 done;
	u32 priority;
} RouteRepairRange;

typedef enum
{
	RANGE_SUPPORT_NO = 0,
	RANGE_SUPPORT_PROBE,
	RANGE_SUPPORT_YES,
} RouteServerRangeSupport;

typedef struct
{
	char *url;
	RouteServerRangeSupport accept_ranges;
	Bool is_up, support_h2;
	u32 nb_req_success, nb_bytes, latency;
} RouteRepairServer;

#define REPAIR_BUF_SIZE	50000
typedef struct
{
	GF_DownloadSession *dld;
	RepairSegmentInfo *current_si;

	RouteRepairRange *range;
	RouteRepairServer *server;
	u32 initial_retry, retry_in;
	char http_buf[REPAIR_BUF_SIZE];
} RouteRepairSession;

typedef struct
{
	//options
	char *src, *ifce, *odir;
	Bool gcache, kc, skipr, reorder, fullseg, cloop, llmode, dynsel;
	u32 buffer, timeout, stats, max_segs, tsidbg, rtimeout, nbcached;
	ROUTEInRepairMode repair;
	u32 max_sess, range_merge, minrecv;
	s32 tunein, stsi;
	GF_PropStringList repair_urls;

	//internal
	GF_Filter *filter;
	GF_DownloadManager *dm;

	char *clock_init_seg;
	GF_ROUTEDmx *route_dmx;
	u32 tune_service_id;

	u32 sync_tsi, last_toi;

	u32 start_time, tune_time, last_timeout;
	GF_FilterPid *opid;
	GF_List *tsi_outs;

	u32 nb_stats;
	GF_List *received_seg_names;

	u32 nb_playing;
	Bool initial_play_forced;
	Bool evt_interrupt;

	RouteRepairSession *http_repair_sessions;

	GF_List *seg_repair_queue;
	GF_List *seg_repair_reservoir;
	GF_List *seg_range_reservoir;
	GF_List *repair_servers;

	Bool has_data;
	const char *log_name;
} ROUTEInCtx;

struct _route_repair_seg_info
{
	//copy of finfo event, valid until associated object is removed
	GF_ROUTEEventFileInfo finfo;
	//copy of filename which is not guaranteed to be kept outside the event callback
	char *filename;
	GF_ROUTEEventType evt;
	u32 service_id;
	Bool removed;
	u32 pending;
	GF_List *ranges;
	u32 nb_errors;
	TSI_Output *tsio;
	//set to true if repair session is over but kept in list for TSIO reordering purposes
	Bool done;
};



void routein_repair_mark_file(ROUTEInCtx *ctx, u32 service_id, const char *filename, Bool is_delete);
void routein_queue_repair(ROUTEInCtx *ctx, GF_ROUTEEventType evt, u32 evt_param, GF_ROUTEEventFileInfo *finfo);

void routein_on_event_file(ROUTEInCtx *ctx, GF_ROUTEEventType evt, u32 evt_param, GF_ROUTEEventFileInfo *finfo, Bool is_defer_repair, Bool drop_if_first);

//return GF_EOS if nothing active, GF_OK otherwise
GF_Err routein_do_repair(ROUTEInCtx *ctx);

TSI_Output *routein_get_tsio(ROUTEInCtx *ctx, u32 service_id, GF_ROUTEEventFileInfo *finfo);

#endif /* GPAC_DISABLE_ROUTE */

#endif //#define IN_ROUTE_H
