/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017-2024
 *					All rights reserved
 *
 *  This file is part of GPAC / generic FILE output filter
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
#include <gpac/constants.h>
#include <gpac/xml.h>
#include <gpac/network.h>
#include <gpac/mpd.h>

#ifndef GPAC_DISABLE_FOUT

GF_OPT_ENUM (GF_FileOutConcatMode,
	FOUT_CAT_NONE = 0,
	FOUT_CAT_AUTO,
	FOUT_CAT_ALL,
);

GF_OPT_ENUM (GF_FileOutOverwriteMode,
	FOUT_OW_YES = 0,
	FOUT_OW_NO,
	FOUT_OW_ASK,
);

typedef struct
{
	//options
	Double start, speed;
	char *dst, *mime, *ext;
	Bool append, dynext, redund, noinitraw, force_null, atomic;
	GF_FileOutConcatMode cat;
	GF_FileOutOverwriteMode ow;
	u32 mvbk;
	s32 max_cache_segs;

	//only one input pid
	GF_FilterPid *pid;

	FILE *file;
	Bool is_std;
	u64 nb_write;
	Bool use_templates;
	GF_FilterCapability in_caps[2];
	char szExt[10];
	char szFileName[GF_MAX_PATH];
	char *llhas_template;

	Bool patch_blocks;
	Bool is_null;
	GF_Err error;
	u32 dash_mode;
	u64 offset_at_seg_start;
	const char *original_url;
	GF_FileIO *gfio_ref;

	FILE *hls_chunk;

	u32 max_segs, llhas_mode;
	GF_List *past_files;

	Bool gfio_pending;

	u64 last_file_size;
	Bool use_rel;
	Bool use_move;
	char *llhls_file_name;

#ifdef GPAC_HAS_FD
	Bool no_fd;
	s32 fd;
#endif
} GF_FileOutCtx;

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define ATOMIC_SUFFIX	".gftmp"

static void fileout_close_hls_chunk(GF_FileOutCtx *ctx, Bool final_flush)
{
	if (!ctx->hls_chunk) return;
	gf_fclose(ctx->hls_chunk);
	ctx->hls_chunk = NULL;
	if (!ctx->llhls_file_name) return;

	char szName[GF_MAX_PATH];
	strcpy(szName, ctx->llhls_file_name);
	strcat(szName, ATOMIC_SUFFIX);
	gf_file_delete(ctx->llhls_file_name);
	gf_file_move(szName, ctx->llhls_file_name);
	gf_free(ctx->llhls_file_name);
	ctx->llhls_file_name = NULL;
}

static void fileout_check_close(GF_FileOutCtx *ctx)
{
	if (!ctx->use_move) return;
	char szName[GF_MAX_PATH];
	strcpy(szName, ctx->szFileName);
	strcat(szName, ATOMIC_SUFFIX);
	gf_file_delete(ctx->szFileName);
	gf_file_move(szName, ctx->szFileName);
	ctx->use_move = GF_FALSE;
}

static GF_Err fileout_open_close(GF_FileOutCtx *ctx, const char *filename, const char *ext, u32 file_idx, Bool explicit_overwrite, char *file_suffix, Bool check_no_open)
{
	if (!ctx->is_std) {
#ifdef GPAC_HAS_FD
		if (ctx->fd>=0) {
			GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[FileOut] closing output file %s\n", ctx->szFileName));
			close(ctx->fd);
			fileout_close_hls_chunk(ctx, GF_FALSE);
		} else
#endif
		 if (ctx->file) {
			GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[FileOut] closing output file %s\n", ctx->szFileName));
			gf_fclose(ctx->file);
			fileout_close_hls_chunk(ctx, GF_FALSE);
		}
		fileout_check_close(ctx);
	}
	ctx->file = NULL;
#ifdef GPAC_HAS_FD
	ctx->fd = -1;
#endif

	if (!filename)
		return GF_OK;

	if (!strcmp(filename, "std")) ctx->is_std = GF_TRUE;
	else if (!strcmp(filename, "stdout")) ctx->is_std = GF_TRUE;
	else ctx->is_std = GF_FALSE;

	if (!strcmp(filename, "null") || !strcmp(filename, "/dev/null"))
		ext = NULL;

	if (ctx->is_std) {
		ctx->file = stdout;
		ctx->nb_write = 0;
#ifdef WIN32
		_setmode(_fileno(stdout), _O_BINARY);
#endif
		return GF_OK;
	}

	char szName[GF_MAX_PATH];
	char szFinalName[GF_MAX_PATH];
	Bool append = ctx->append;
	Bool is_gfio=GF_FALSE;
	const char *url = filename;

	if (!strncmp(filename, "gfio://", 7)) {
		url = gf_fileio_translate_url(filename);
		is_gfio=GF_TRUE;
	}

	if (ctx->dynext) {
		const char *has_ext = gf_file_ext_start(url);

		strcpy(szFinalName, url);
		if (!has_ext && ext) {
			strcat(szFinalName, ".");
			strcat(szFinalName, ext);
		}
	} else {
		strcpy(szFinalName, url);
	}

	if (ctx->use_templates) {
		GF_Err e;
		gf_assert(ctx->dst);
		if (!strcmp(filename, ctx->dst)) {
			strcpy(szName, szFinalName);
			e = gf_filter_pid_resolve_file_template(ctx->pid, szName, szFinalName, file_idx, file_suffix);
		} else {
			char szFileName[GF_MAX_PATH];
			strcpy(szFileName, szFinalName);
			strcpy(szName, ctx->dst);
			e = gf_filter_pid_resolve_file_template_ex(ctx->pid, szName, szFinalName, file_idx, file_suffix, szFileName);
		}
		if (e) {
			return ctx->error = e;
		}
	}

	if (!gf_file_exists(szFinalName)) append = GF_FALSE;

	if (!strcmp(szFinalName, ctx->szFileName) && (ctx->cat==FOUT_CAT_AUTO))
		append = GF_TRUE;

	if (!append && (ctx->ow!=FOUT_OW_YES) && gf_file_exists(szFinalName)) {
		char szRes[21];
		s32 res;

		if (ctx->ow==FOUT_OW_ASK) {
			fprintf(stderr, "File %s already exists - override (y/n/a) ?:", szFinalName);
			res = scanf("%20s", szRes);
			if (!res || (szRes[0] == 'n') || (szRes[0] == 'N')) {
				return ctx->error = GF_IO_ERR;
			}
			if ((szRes[0] == 'a') || (szRes[0] == 'A')) ctx->ow = FOUT_OW_YES;
		} else {
			return ctx->error = GF_IO_ERR;
		}
	}

	if (check_no_open && (ctx->llhas_mode==GF_LLHAS_SUBSEG)) {
		strcpy(ctx->szFileName, szFinalName);
		ctx->nb_write = 0;
		return GF_OK;
	}

	GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[FileOut] opening output file %s\n", szFinalName));

	ctx->use_move = GF_FALSE;
	if (ctx->atomic && !append && !is_gfio && (!ctx->original_url || strncmp(ctx->original_url, "gfio://", 7))) {
		ctx->use_move = GF_TRUE;
	}
	strcpy(szName, szFinalName);
	if (ctx->use_move) {
		strcat(szName, ATOMIC_SUFFIX);
	}

#ifdef GPAC_HAS_FD
	if (!ctx->no_fd && !is_gfio && !append && !gf_opts_get_bool("core", "no-fd")
		&& (!ctx->original_url || strncmp(ctx->original_url, "gfio://", 7))
	) {
		//make sure output dir exists
		gf_fopen(szFinalName, "mkdir");

		ctx->fd = gf_fd_open(szName, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH );
	} else
#endif
		ctx->file = gf_fopen_ex(szName, ctx->original_url, append ? "a+b" : "w+b", GF_FALSE);

	if (!strcmp(szFinalName, ctx->szFileName) && !append && ctx->nb_write && !explicit_overwrite) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[FileOut] re-opening in write mode output file %s, content overwrite (use `cat` option to enable append)\n", szFinalName));
	}
	strcpy(ctx->szFileName, szFinalName);

	ctx->nb_write = 0;
	if (!ctx->file
#ifdef GPAC_HAS_FD
		&& (ctx->fd<0)
#endif
	) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] cannot open output file %s\n", ctx->szFileName));
		return ctx->error = GF_IO_ERR;
	}

	return GF_OK;
}

static void fileout_setup_file(GF_FileOutCtx *ctx, Bool explicit_overwrite)
{
	const char *dst = ctx->dst;
	const GF_PropertyValue *p, *ext;

	p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_OUTPATH);
	ext = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_FILE_EXT);

	if (p && p->value.string) {
		fileout_open_close(ctx, p->value.string, (ext && ctx->dynext) ? ext->value.string : NULL, 0, explicit_overwrite, NULL, GF_FALSE);
		return;
	}
	if (!dst) {
		p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_FILEPATH);
		if (p && p->value.string) {
			dst = p->value.string;
			char *sep = strstr(dst, "://");
			if (sep) {
				dst = strchr(sep+3, '/');
				if (!dst) return;
			} else {
				if (!strncmp(dst, "./", 2)) dst+= 2;
				else if (!strncmp(dst, ".\\", 2)) dst+= 2;
				else if (!strncmp(dst, "../", 3)) dst+= 3;
				else if (!strncmp(dst, "..\\", 3)) dst+= 3;
			}
		}
	}
	if (ctx->dynext) {
		if (ext && ext->value.string) {
			fileout_open_close(ctx, dst, ext->value.string, 0, explicit_overwrite, NULL, GF_FALSE);
		}
	} else if (ctx->dst) {
		fileout_open_close(ctx, ctx->dst, NULL, 0, explicit_overwrite, NULL, GF_FALSE);
	} else {
		p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_FILEPATH);
		if (!p) p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_URL);
		if (p && p->value.string)
			fileout_open_close(ctx, p->value.string, NULL, 0, explicit_overwrite, NULL, GF_FALSE);
	}
}
static GF_Err fileout_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	GF_FileOutCtx *ctx = (GF_FileOutCtx *) gf_filter_get_udta(filter);
	if (is_remove) {
		ctx->pid = NULL;
		fileout_open_close(ctx, NULL, NULL, 0, GF_FALSE, NULL, GF_FALSE);
		return GF_OK;
	}
	gf_filter_pid_check_caps(pid);

	if (!ctx->pid) {
		GF_FilterEvent evt;
		gf_filter_pid_init_play_event(pid, &evt, ctx->start, ctx->speed, "FileOut");
		gf_filter_pid_send_event(pid, &evt);
	}
	ctx->pid = pid;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_DISABLE_PROGRESSIVE);
	if (p && p->value.uint) ctx->patch_blocks = GF_TRUE;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_DASH_MODE);
	if (p && p->value.uint) ctx->dash_mode = 1;

	ctx->max_segs = 0;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_IS_MANIFEST);
	//do not cleanup manifest pids
	if (!p || !p->value.uint) {
		p = gf_filter_pid_get_property(pid, GF_PROP_PID_TIMESHIFT_SEGS);
		if (ctx->max_cache_segs<0) {
			ctx->max_segs = (u32) -ctx->max_cache_segs;
		} else if (ctx->max_cache_segs>0) {
			ctx->max_segs = (u32) ctx->max_cache_segs;
			if (p && (p->value.uint > (u32) ctx->max_cache_segs))
				ctx->max_segs = p->value.uint;
		}
		if (ctx->max_segs && !ctx->past_files)
			ctx->past_files = gf_list_new();
	}
	p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_LLHAS_MODE);
	ctx->llhas_mode = p ? p->value.uint : GF_LLHAS_NONE;

#ifdef GPAC_HAS_FD
	//disable fd for mp2t since we only dispatch small blocks - todo check this for other streams ?
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (p && (p->value.uint==GF_CODECID_FAKE_MP2T)) ctx->no_fd = GF_TRUE;
#endif

	ctx->error = GF_OK;
	return GF_OK;
}

static GF_Err fileout_initialize(GF_Filter *filter)
{
	char *ext=NULL, *sep;
	const char *dst;
	GF_FileOutCtx *ctx = (GF_FileOutCtx *) gf_filter_get_udta(filter);

	if (!ctx || !ctx->dst) return GF_OK;

	if (!ctx->mvbk)
		ctx->mvbk = 1;

#ifdef GPAC_HAS_FD
	ctx->fd = -1;
#endif

	if (strnicmp(ctx->dst, "file:/", 6) && strnicmp(ctx->dst, "gfio:/", 6) && strstr(ctx->dst, "://"))  {
		gf_filter_setup_failure(filter, GF_NOT_SUPPORTED);
		return GF_NOT_SUPPORTED;
	}
	if (!stricmp(ctx->dst, "null") ) {
		ctx->is_null = GF_TRUE;
		//null and no format specified, we accept any kind
		if (!ctx->ext) {
			ctx->in_caps[0].code = GF_PROP_PID_STREAM_TYPE;
			ctx->in_caps[0].val = PROP_UINT(GF_STREAM_UNKNOWN);
			ctx->in_caps[0].flags = GF_CAPS_INPUT_EXCLUDED;
			gf_filter_override_caps(filter, ctx->in_caps, 1);
			return GF_OK;
		}
	}
	if (!strncmp(ctx->dst, "gfio://", 7)) {
		GF_Err e;
		ctx->gfio_ref = gf_fileio_open_url(gf_fileio_from_url(ctx->dst), NULL, "ref", &e);
		if (!ctx->gfio_ref) {
			gf_filter_setup_failure(filter, e);
			return e;
		}
		dst = gf_fileio_translate_url(ctx->dst);
		ctx->original_url = ctx->dst;
	} else {
		dst = ctx->dst;
	}

	sep = dst ? strchr(dst, '$') : NULL;
	if (sep) {
		sep = strchr(sep+1, '$');
		if (sep) ctx->use_templates = GF_TRUE;
	}

	if (ctx->dynext) return GF_OK;

	if (ctx->ext) ext = ctx->ext;
	else if (dst) {
		ext = gf_file_ext_start(dst);
		if (ext) ext += 1;
	}

	if (!ext && !ctx->mime) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] No extension provided nor mime type for output file %s, cannot infer format\n", ctx->dst));
		return GF_NOT_SUPPORTED;
	}
	//static cap, streamtype = file
	ctx->in_caps[0].code = GF_PROP_PID_STREAM_TYPE;
	ctx->in_caps[0].val = PROP_UINT(GF_STREAM_FILE);
	ctx->in_caps[0].flags = GF_CAPS_INPUT_STATIC;

	if (ctx->mime) {
		ctx->in_caps[1].code = GF_PROP_PID_MIME;
		ctx->in_caps[1].val = PROP_NAME( ctx->mime );
		ctx->in_caps[1].flags = GF_CAPS_INPUT;
	} else {
		strncpy(ctx->szExt, ext, 9);
		ctx->szExt[9] = 0;
		strlwr(ctx->szExt);
		ctx->in_caps[1].code = GF_PROP_PID_FILE_EXT;
		ctx->in_caps[1].val = PROP_NAME( ctx->szExt );
		ctx->in_caps[1].flags = GF_CAPS_INPUT;
	}
	gf_filter_override_caps(filter, ctx->in_caps, 2);

	if (ctx->force_null) {
		ctx->is_null = GF_TRUE;
	}
	return GF_OK;
}

static void fileout_finalize(GF_Filter *filter)
{
	GF_Err e;
	GF_FileOutCtx *ctx = (GF_FileOutCtx *) gf_filter_get_udta(filter);

	fileout_close_hls_chunk(ctx, GF_TRUE);

	fileout_open_close(ctx, NULL, NULL, 0, GF_FALSE, NULL, GF_FALSE);
	if (ctx->gfio_ref)
		gf_fileio_open_url((GF_FileIO *)ctx->gfio_ref, NULL, "unref", &e);

	if (ctx->past_files) {
		while (gf_list_count(ctx->past_files)) {
			char *url = gf_list_pop_back(ctx->past_files);
			gf_free(url);
		}
		gf_list_del(ctx->past_files);
	}
	if (ctx->llhas_template) gf_free(ctx->llhas_template);
	if (ctx->llhls_file_name) gf_free(ctx->llhls_file_name);
}

static GF_Err fileout_process(GF_Filter *filter)
{
	GF_Err e=GF_OK;
	GF_FilterPacket *pck;
	const GF_PropertyValue *fname, *p;
	Bool start, end;
	const u8 *pck_data;
	u32 pck_size, nb_write;
	GF_FileOutCtx *ctx = (GF_FileOutCtx *) gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->pid);

restart:

	if (ctx->error)
		return ctx->error;

	if (!pck) {
		if (gf_filter_pid_is_eos(ctx->pid) && !gf_filter_pid_is_flush_eos(ctx->pid)) {
			if (gf_filter_reporting_enabled(filter)) {
				char szStatus[1024];
				snprintf(szStatus, 1024, "%s: done - wrote "LLU" bytes", gf_file_basename(ctx->szFileName), ctx->nb_write);
				gf_filter_update_status(filter, 10000, szStatus);
			}

			if (ctx->dash_mode && (ctx->file || ctx->last_file_size
#ifdef GPAC_HAS_FD
				|| (ctx->fd>=0)
#endif
			)) {
				GF_FilterEvent evt;
				GF_FEVT_INIT(evt, GF_FEVT_SEGMENT_SIZE, ctx->pid);
				evt.seg_size.seg_url = NULL;

				if (ctx->dash_mode==1) {
					evt.seg_size.is_init = 1;
					ctx->dash_mode = 2;
					evt.seg_size.media_range_start = 0;
					evt.seg_size.media_range_end = 0;
					gf_filter_pid_send_event(ctx->pid, &evt);
				} else {
					evt.seg_size.is_init = 0;
					evt.seg_size.media_range_start = ctx->offset_at_seg_start;
#ifdef GPAC_HAS_FD
					if (ctx->fd>=0) {
						evt.seg_size.media_range_end = lseek_64(ctx->fd, 0, SEEK_CUR);
					} else
#endif
					if (ctx->file) {
						evt.seg_size.media_range_end = gf_ftell(ctx->file);
					} else {
						evt.seg_size.media_range_end = ctx->last_file_size;
					}
					//end range excludes last byte, except if 0 size (some text segments)
					if (evt.seg_size.media_range_end)
						evt.seg_size.media_range_end -= 1;

					gf_filter_pid_send_event(ctx->pid, &evt);
				}
			}
			fileout_open_close(ctx, NULL, NULL, 0, GF_FALSE, NULL, GF_FALSE);
			return GF_EOS;
		}
		return GF_OK;
	}

	gf_filter_pck_get_framing(pck, &start, &end);
	if (!ctx->redund && !ctx->dash_mode) {
		u32 dep_flags = gf_filter_pck_get_dependency_flags(pck);
		//redundant packet, do not store
		if ((dep_flags & 0x3) == 1) {
			gf_filter_pid_drop_packet(ctx->pid);
			return GF_OK;
		}
	}

	if (ctx->is_null) {
		if (start) {
			u32 fnum=0;
			const char *filename=NULL;
			p = gf_filter_pck_get_property(pck, GF_PROP_PCK_FILENUM);
			if (p) fnum = p->value.uint;
			p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_URL);
			if (!p) p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_OUTPATH);
			if (!p) p = gf_filter_pck_get_property(pck, GF_PROP_PCK_FILENAME);
			if (p) filename = p->value.string;
			if (filename) {
				strcpy(ctx->szFileName, filename);
			} else {
				sprintf(ctx->szFileName, "%d", fnum);
			}
			GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[FileOut] null open (file name is %s)\n", ctx->szFileName));
		}
		if (end) {
			GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[FileOut] null close (file name was %s)\n", ctx->szFileName));
		}
		gf_filter_pid_drop_packet(ctx->pid);
		pck = gf_filter_pid_get_packet(ctx->pid);
		goto restart;
	}

	if (ctx->gfio_pending) goto check_gfio;

	if (start && (ctx->cat==FOUT_CAT_ALL)
		&& (ctx->file
#ifdef GPAC_HAS_FD
			|| (ctx->fd>=0)
#endif
		)
	)
		start = GF_FALSE;

	if (ctx->dash_mode) {
		p = gf_filter_pck_get_property(pck, GF_PROP_PCK_FILENUM);
		if (p) {
			GF_FilterEvent evt;

			GF_FEVT_INIT(evt, GF_FEVT_SEGMENT_SIZE, ctx->pid);
			evt.seg_size.seg_url = NULL;

			if (ctx->dash_mode==1) {
				evt.seg_size.is_init = 1;
				ctx->dash_mode = 2;
				evt.seg_size.media_range_start = 0;
				evt.seg_size.media_range_end = 0;
				gf_filter_pid_send_event(ctx->pid, &evt);
			} else {
				evt.seg_size.is_init = 0;
				evt.seg_size.media_range_start = ctx->offset_at_seg_start;
#ifdef GPAC_HAS_FD
				if (ctx->fd>=0) {
					evt.seg_size.media_range_end = lseek_64(ctx->fd, 0, SEEK_CUR);
				} else
#endif
				if (ctx->file) {
					evt.seg_size.media_range_end = gf_ftell(ctx->file);
				} else {
					evt.seg_size.media_range_end = ctx->last_file_size;
				}
				//end range excludes last byte, except if 0 size (some text segments)
				if (evt.seg_size.media_range_end)
					evt.seg_size.media_range_end -= 1;

				ctx->offset_at_seg_start = evt.seg_size.media_range_end+1;
				gf_filter_pid_send_event(ctx->pid, &evt);
			}
			if ( gf_filter_pck_get_property(pck, GF_PROP_PCK_FILENAME))
				start = GF_TRUE;
		}

		p = gf_filter_pck_get_property(pck, GF_PROP_PCK_EODS);
		if (p && p->value.boolean) {
			end = GF_TRUE;
		}
	}

	if (start) {
		const GF_PropertyValue *ext, *fnum, *fsuf, *rel;
		Bool explicit_overwrite = GF_FALSE;
		const char *name = NULL;
		fname = ext = NULL;
		ctx->last_file_size = 0;
		//file num increased per packet, open new file
		fnum = gf_filter_pck_get_property(pck, GF_PROP_PCK_FILENUM);
		if (fnum) {
			fname = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_OUTPATH);
			ext = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_FILE_EXT);
			if (!fname) name = ctx->dst;
		}
		//filename change at packet start, open new file
		if (!fname) fname = gf_filter_pck_get_property(pck, GF_PROP_PCK_FILENAME);
		if (fname) name = fname->value.string;

		fsuf = gf_filter_pck_get_property(pck, GF_PROP_PCK_FILESUF);

		if (end && gf_filter_pck_get_seek_flag(pck))
			explicit_overwrite = GF_TRUE;

		if (name) {
			Bool use_rel = GF_FALSE;
			if (ctx->dst) {
				use_rel = ctx->use_rel;
				rel = gf_filter_pck_get_property(pck, GF_PROP_PCK_FILE_REL);
				if (rel && rel->value.boolean) use_rel = GF_TRUE;
			}
			if (use_rel) {
				name = gf_url_concatenate(ctx->dst, name);
			}
			fileout_open_close(ctx, name, ext ? ext->value.string : NULL, fnum ? fnum->value.uint : 0, explicit_overwrite, fsuf ? fsuf->value.string : NULL, GF_TRUE);

			if (use_rel) {
				gf_free((char*) name);
			}

		} else if (!ctx->file && !ctx->noinitraw
#ifdef GPAC_HAS_FD
			&& (ctx->fd<0)
#endif
		) {
			fileout_setup_file(ctx, explicit_overwrite);
		}
		if (!ctx->cat)
			ctx->offset_at_seg_start = 0;

		if (gf_fileio_check(ctx->file)) {
			ctx->gfio_pending = GF_TRUE;
		}

		fname = gf_filter_pck_get_property(pck, GF_PROP_PCK_LLHAS_TEMPLATE);
		if (fname) {
			if (ctx->llhas_template) gf_free(ctx->llhas_template);
			ctx->llhas_template = gf_strdup(fname->value.string);
		}

		if (ctx->max_segs) {
			while (gf_list_count(ctx->past_files)>ctx->max_segs) {
				char *url = gf_list_pop_front(ctx->past_files);
				gf_file_delete(url);
				gf_free(url);
			}
			p = gf_filter_pck_get_property(pck, GF_PROP_PCK_INIT);
			if (!p || !p->value.boolean) {
				gf_list_add(ctx->past_files, gf_strdup(ctx->szFileName));
			}
		}
	}
	p = gf_filter_pck_get_property(pck, GF_PROP_PCK_LLHAS_FRAG_NUM);
	if (p) {
#ifndef GPAC_DISABLE_MPD
		char *llhas_chunkname = gf_mpd_resolve_subnumber(ctx->llhas_template, ctx->szFileName, p->value.uint);
		//for now we only use buffered IO for hls chunks, too small to really benefit from direct write
		fileout_close_hls_chunk(ctx, GF_FALSE);
		if (ctx->use_move) {
			ctx->llhls_file_name = gf_strdup(llhas_chunkname);
			gf_dynstrcat(&llhas_chunkname, ATOMIC_SUFFIX, NULL);
			ctx->hls_chunk = gf_fopen_ex(llhas_chunkname, ctx->original_url, "w+b", GF_FALSE);
		} else {
			ctx->hls_chunk = gf_fopen_ex(llhas_chunkname, ctx->original_url, "w+b", GF_FALSE);
		}
		ctx->gfio_pending = GF_TRUE;
		gf_free(llhas_chunkname);
#else
		gf_filter_setup_failure(filter, GF_NOT_SUPPORTED);
		return GF_NOT_SUPPORTED;
#endif
	}

check_gfio:
	if (ctx->gfio_pending) {
		GF_FileIOWriteState wstate = gf_fileio_write_ready(ctx->file);
		if ((wstate==GF_FIO_WRITE_READY) && ctx->hls_chunk)
			wstate = gf_fileio_write_ready(ctx->hls_chunk);

		if (wstate==GF_FIO_WRITE_WAIT) {
			ctx->gfio_pending = GF_TRUE;
			return GF_OK;
		} else if (wstate==GF_FIO_WRITE_CANCELED) {
			gf_filter_abort(filter);
			ctx->gfio_pending = GF_FALSE;
			return GF_OK;
		}
		ctx->gfio_pending = GF_FALSE;
	}


	Bool main_valid = 0;
	if (ctx->file
#ifdef GPAC_HAS_FD
		|| (ctx->fd>=0)
#endif
	)
		main_valid = GF_TRUE;

	pck_data = gf_filter_pck_get_data(pck, &pck_size);
	if (main_valid || ctx->hls_chunk) {
		GF_FilterFrameInterface *hwf = gf_filter_pck_get_frame_interface(pck);
		if (pck_data) {
			if (ctx->patch_blocks && gf_filter_pck_get_seek_flag(pck) && main_valid) {
				u64 bo = gf_filter_pck_get_byte_offset(pck);
				if (ctx->is_std) {
					GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Cannot patch file, output is stdout\n"));
				} else if (bo==GF_FILTER_NO_BO) {
					GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Cannot patch file, wrong byte offset\n"));
				} else {
					u32 ilaced = gf_filter_pck_get_interlaced(pck);
					u64 pos = ctx->nb_write;

					//we are inserting a block: write dummy bytes at end and move bytes
					if (ilaced) {
						u8 *block;
						u64 cur_r, cur_w;
#ifdef GPAC_HAS_FD
						if (ctx->fd>=0) {
							nb_write = (u32) write(ctx->fd, pck_data, pck_size);
							cur_w = lseek_64(ctx->fd, 0, SEEK_CUR);
							lseek_64(ctx->fd, pos, SEEK_SET);
						} else
#endif
						{
							nb_write = (u32) gf_fwrite(pck_data, pck_size, ctx->file);
							cur_w = gf_ftell(ctx->file);
							gf_fseek(ctx->file, pos, SEEK_SET);
						}

						if (nb_write!=pck_size) {
							GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Write error, wrote %d bytes but had %d to write\n", nb_write, pck_size));
							e = GF_IO_ERR;
						}

						cur_r = pos;
						pos = cur_w;
						block = gf_malloc(ctx->mvbk);
						if (!block) {
							GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] unable to allocate block of %d bytes\n", ctx->mvbk));
							e = GF_IO_ERR;
						} else {
							while (cur_r > bo) {
								u32 move_bytes = ctx->mvbk;
								if (cur_r - bo < move_bytes)
									move_bytes = (u32) (cur_r - bo);

#ifdef GPAC_HAS_FD
								if (ctx->fd>=0) {
									lseek_64(ctx->fd, cur_r - move_bytes, SEEK_SET);
									nb_write = (u32) read(ctx->fd, block, (size_t) move_bytes);
								} else
#endif
								{
									gf_fseek(ctx->file, cur_r - move_bytes, SEEK_SET);
									nb_write = (u32) gf_fread(block, (size_t) move_bytes, ctx->file);
								}

								if (nb_write!=move_bytes) {
									GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Read error, got %d bytes but had %d to read\n", nb_write, move_bytes));
									e = GF_IO_ERR;
								}

#ifdef GPAC_HAS_FD
								if (ctx->fd>=0) {
									lseek_64(ctx->fd, cur_w - move_bytes, SEEK_SET);
									nb_write = (u32) write(ctx->fd, block, (size_t) move_bytes);
								} else
#endif
								{
									gf_fseek(ctx->file, cur_w - move_bytes, SEEK_SET);
									nb_write = (u32) gf_fwrite(block, (size_t) move_bytes, ctx->file);
								}

								if (nb_write!=move_bytes) {
									GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Write error, wrote %d bytes but had %d to write\n", nb_write, move_bytes));
									e = GF_IO_ERR;
								}
								cur_r -= move_bytes;
								cur_w -= move_bytes;
							}
							gf_free(block);
						}
					}

#ifdef GPAC_HAS_FD
					if (ctx->fd>=0) {
						lseek_64(ctx->fd, bo, SEEK_SET);
						nb_write = (u32) write(ctx->fd, pck_data, pck_size);
						lseek_64(ctx->fd, pos, SEEK_SET);
					} else
#endif
					{
						gf_fseek(ctx->file, bo, SEEK_SET);
						nb_write = (u32) gf_fwrite(pck_data, pck_size, ctx->file);
						gf_fseek(ctx->file, pos, SEEK_SET);
					}

					if (nb_write!=pck_size) {
						GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Write error, wrote %d bytes but had %d to write\n", nb_write, pck_size));
						e = GF_IO_ERR;
					}
				}
			} else {
				if (main_valid) {

#ifdef GPAC_HAS_FD
					if (ctx->fd>=0) {
						nb_write = (u32) write(ctx->fd, pck_data, pck_size);
					} else
#endif
						nb_write = (u32) gf_fwrite(pck_data, pck_size, ctx->file);

					if (nb_write!=pck_size) {
						GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Write error, wrote %d bytes but had %d to write\n", nb_write, pck_size));
						e = GF_IO_ERR;
					}
					ctx->nb_write += nb_write;
				}

				if (ctx->hls_chunk) {
					nb_write = (u32) gf_fwrite(pck_data, pck_size, ctx->hls_chunk);
					if (nb_write!=pck_size) {
						GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Write error, wrote %d bytes but had %d to write\n", nb_write, pck_size));
						e = GF_IO_ERR;
					}
					if (!main_valid)
						ctx->nb_write += nb_write;
				}
			}
		} else if (hwf && main_valid) {
			u32 w, h, stride, stride_uv, pf;
			u32 nb_planes, uv_height;
			p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_WIDTH);
			w = p ? p->value.uint : 0;
			p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_HEIGHT);
			h = p ? p->value.uint : 0;
			p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_PIXFMT);
			pf = p ? p->value.uint : 0;

			stride = stride_uv = 0;

			if (gf_pixel_get_size_info(pf, w, h, NULL, &stride, &stride_uv, &nb_planes, &uv_height) == GF_TRUE) {
				u32 i;
				for (i=0; i<nb_planes; i++) {
					u32 j, write_h, lsize;
					const u8 *out_ptr;
					u32 out_stride = i ? stride_uv : stride;
					e = hwf->get_plane(hwf, i, &out_ptr, &out_stride);
					if (e) {
						GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Failed to fetch plane data from hardware frame, cannot write\n"));
						break;
					}
					if (i) {
						write_h = uv_height;
						lsize = stride_uv;
					} else {
						write_h = h;
						lsize = stride;
					}
					for (j=0; j<write_h; j++) {
#ifdef GPAC_HAS_FD
						if (ctx->fd>=0) {
							nb_write = (u32) write(ctx->fd, out_ptr, lsize);
						} else
#endif
							nb_write = (u32) gf_fwrite(out_ptr, lsize, ctx->file);
						if (nb_write!=lsize) {
							GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] Write error, wrote %d bytes but had %d to write\n", nb_write, lsize));
							e = GF_IO_ERR;
						}
						ctx->nb_write += nb_write;
						out_ptr += out_stride;
					}
				}
			}
		} else if (!main_valid && pck_size) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] output file handle is not opened, discarding %d bytes\n", pck_size));
		} else {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[FileOut] No data associated with packet, cannot write\n"));
		}
	} else if (pck_size) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[FileOut] output file handle is not opened, discarding %d bytes\n", pck_size));
	}
	gf_filter_pid_drop_packet(ctx->pid);
	if (end && !ctx->cat) {
		if (ctx->dash_mode) {
#ifdef GPAC_HAS_FD
			if (ctx->fd>=0) {
				ctx->last_file_size = lseek_64(ctx->fd, 0, SEEK_CUR);
			} else
#endif
				ctx->last_file_size = gf_ftell(ctx->file);
		}
		fileout_open_close(ctx, NULL, NULL, 0, GF_FALSE, NULL, GF_FALSE);
	}
	pck = gf_filter_pid_get_packet(ctx->pid);
	if (pck)
		goto restart;

	if (gf_filter_reporting_enabled(filter)) {
		char szStatus[1024];
		snprintf(szStatus, 1024, "%s: wrote % 16"LLD_SUF" bytes", gf_file_basename(ctx->szFileName), (s64) ctx->nb_write);
		gf_filter_update_status(filter, -1, szStatus);
	}
	return e;
}

static Bool fileout_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	if (evt->base.type==GF_FEVT_FILE_DELETE) {
		GF_FileOutCtx *ctx = (GF_FileOutCtx *) gf_filter_get_udta(filter);
		if (ctx->is_null) {
			GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[FileOut] null delete (file name was %s)\n", evt->file_del.url));
		} else {
			GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[FileOut] delete file %s\n", evt->file_del.url));
			if (ctx->use_rel) {
				char *fname = gf_url_concatenate(ctx->dst, evt->file_del.url);
				gf_file_delete(fname);
				gf_free(fname);
			} else {
				gf_file_delete(evt->file_del.url);
			}
		}
		return GF_TRUE;
	}
	return GF_FALSE;
}
static GF_FilterProbeScore fileout_probe_url(const char *url, const char *mime)
{
	if (strstr(url, "://")) {

		if (!strnicmp(url, "file://", 7)) return GF_FPROBE_MAYBE_SUPPORTED;
		if (!strnicmp(url, "gfio://", 7)) {
			if (!gf_fileio_write_mode(gf_fileio_from_url(url)))
				return GF_FPROBE_NOT_SUPPORTED;
			return GF_FPROBE_MAYBE_SUPPORTED;
		}
		return GF_FPROBE_NOT_SUPPORTED;
	}
	return GF_FPROBE_MAYBE_SUPPORTED;
}


#define OFFS(_n)	#_n, offsetof(GF_FileOutCtx, _n)

static const GF_FilterArgs FileOutArgs[] =
{
	{ OFFS(dst), "location of destination file", GF_PROP_NAME, NULL, NULL, 0},
	{ OFFS(append), "open in append mode", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(dynext), "indicate the file extension is set by filter chain, not dst", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(start), "set playback start offset. A negative value means percent of media duration with -1 equal to duration", GF_PROP_DOUBLE, "0.0", NULL, 0},
	{ OFFS(speed), "set playback speed when vsync is on. If negative and start is 0, start is set to -1", GF_PROP_DOUBLE, "1.0", NULL, 0},
	{ OFFS(ext), "set extension for graph resolution, regardless of file extension", GF_PROP_NAME, NULL, NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(mime), "set mime type for graph resolution", GF_PROP_NAME, NULL, NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(cat), "cat each file of input PID rather than creating one file per filename\n"
			"- none: never cat files\n"
			"- auto: only cat if files have same names\n"
			"- all: always cat regardless of file names"
	, GF_PROP_UINT, "none", "none|auto|all", GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(ow), "overwrite output mode when concatenation is not used\n"
	"- yes: override file if existing\n"
	"- no: throw error if file existing\n"
	"- ask: interactive prompt", GF_PROP_UINT, "yes", "yes|no|ask", 0},
	{ OFFS(mvbk), "block size used when moving parts of the file around in patch mode", GF_PROP_UINT, "8192", NULL, 0},
	{ OFFS(redund), "keep redundant packet in output file", GF_PROP_BOOL, "false", NULL, 0},
	{ OFFS(noinitraw), "do not produce initial segment", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_HIDE},
	{ OFFS(max_cache_segs), "maximum number of segments cached per HAS quality when recording live sessions (0 means no limit)", GF_PROP_SINT, "0", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(force_null), "force no output regardless of file name", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(atomic), "use atomic file write for non append modes", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(use_rel), "packet filename use relative names (only set by dasher)", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_HIDE},
	{0}
};

static const GF_FilterCapability FileOutCaps[] =
{
	CAP_UINT(GF_CAPS_INPUT,GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_STRING(GF_CAPS_INPUT,GF_PROP_PID_FILE_EXT, "*"),
};


GF_FilterRegister FileOutRegister = {
	.name = "fout",
	GF_FS_SET_DESCRIPTION("File output")
	GF_FS_SET_HELP("This filter is used to write data to disk, and does not produce any output PID.\n"
		"In regular mode, the filter only accept PID of type file. It will dump to file incoming packets (stream type file), starting a new file for each packet having a __frame_start__ flag set, unless operating in [-cat]() mode.\n"
		"If the output file name is `std` or `stdout`, writes to stdout.\n"
		"The output file name can use gpac templating mechanism, see `gpac -h doc`."
		"The filter watches the property `FileNumber` on incoming packets to create new files.\n"
		"\n"
		"By default output files are created directly, which may lead to issues if concourrent programs attempt to access them.\n"
		"By enabling [-atomic](), files will be created in target destination folder with the `"ATOMIC_SUFFIX"` suffix and move to their final name upon close.\n"
		"\n"
		"# Discard sink mode\n"
		"When the destination is `null`, the filter is a sink dropping all input packets.\n"
		"In this case it accepts ANY type of input PID, not just file ones.\n"
		"\n"
		"# HTTP streaming recording\n"
		"When recording a DASH or HLS session, the number of segments to keep per quality can be set using [-max_cache_segs]().\n"
		"- value `0`  keeps everything (default behaviour)\n"
		"- a negative value `N` will keep `-N` files regardless of the time-shift buffer value\n"
		"- a positive value `N` will keep `MAX(N, time-shift buffer)` files\n"
		"\n"
		"EX gpac -i LIVE_MPD dashin:forward=file -o rec/$File$:max_cache_segs=3\n"
		"This will force keeping a maximum of 3 media segments while recording the DASH session.\n"
		""
	)
	.private_size = sizeof(GF_FileOutCtx),
	.args = FileOutArgs,
	.flags = GF_FS_REG_FORCE_REMUX | GF_FS_REG_TEMP_INIT,
	SETCAPS(FileOutCaps),
	.probe_url = fileout_probe_url,
	.initialize = fileout_initialize,
	.finalize = fileout_finalize,
	.configure_pid = fileout_configure_pid,
	.process = fileout_process,
	.process_event = fileout_process_event,
	.hint_class_type = GF_FS_CLASS_NETWORK_IO
};


const GF_FilterRegister *fout_register(GF_FilterSession *session)
{
	if (gf_opts_get_bool("temp", "get_proto_schemes")) {
		gf_opts_set_key("temp_out_proto", FileOutRegister.name, "file,gfio");
	}
	return &FileOutRegister;
}
#else
const GF_FilterRegister *fout_register(GF_FilterSession *session)
{
	return NULL;
}
#endif //GPAC_DISABLE_FOUT
