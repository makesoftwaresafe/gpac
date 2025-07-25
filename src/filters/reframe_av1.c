/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Romain Bouqueau, Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018-2025
 *					All rights reserved
 *
 *  This file is part of GPAC / AV1 IVF/OBU/annexB reframer filter
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

#include <gpac/avparse.h>
#include <gpac/constants.h>
#include <gpac/filters.h>
#include <gpac/internal/media_dev.h>

#if !defined(GPAC_DISABLE_AV_PARSERS) && !defined(GPAC_DISABLE_RFAV1)

typedef struct
{
	u64 pos;
	Double duration;
} AV1Idx;

typedef enum {
	//input format not probed yet
	NOT_SET,
	//input is AV1 Section 5
	OBUs,
	//input is AV1 annexB
	AnnexB,
	//input is IVF (AV1, vpX ...)
	IVF,
	//input is raw VPX
	RAW_VPX,
	//input is IAMF
	IAMF,
	UNSUPPORTED
} AV1BitstreamSyntax;

typedef struct
{
	//filter args
	GF_Fraction fps;
	Double index;
	Bool importer;
	Bool deps, notime, temporal_delim;

	u32 bsdbg;

	//only one input pid declared
	GF_FilterPid *ipid;
	//only one output pid declared
	GF_FilterPid *opid;

	AV1BitstreamSyntax bsmode;

	GF_BitStream *bs;
	u64 cts;
	u32 width, height;
	GF_Fraction64 duration;
	Double start_range;
	Bool in_seek;
	u32 timescale;
	GF_Fraction cur_fps;

	u32 resume_from;

	char *buffer;
	u32 buf_size, alloc_size;

	//ivf header for now
	u32 file_hdr_size;

	Bool is_av1;
	Bool is_vp9;
	Bool is_iamf;
	u32 codecid;
	u32 num_frames;
	GF_VPConfig *vp_cfg;

	Bool is_playing;
	Bool is_file, file_loaded;
	Bool initial_play_done;

	GF_FilterPacket *src_pck;

	AV1Idx *indexes;
	u32 index_alloc_size, index_size;

	AV1State state;
	IAMFState iamfstate;
	u32 dsi_crc;

	Bool pts_from_file;
	u64 cumulated_dur, last_pts;
	u32 bitrate;

	u32 clli_crc, mdcv_crc;
	Bool copy_props;

	GF_SEILoader *sei_loader;
} GF_AV1DmxCtx;


GF_Err av1dmx_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	GF_AV1DmxCtx *ctx = gf_filter_get_udta(filter);

	if (is_remove) {
		ctx->ipid = NULL;
		if (ctx->opid) {
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		return GF_OK;
	}
	if (! gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_TIMESCALE);
	if (p) ctx->timescale = p->value.uint;
	ctx->state.mem_mode = GF_TRUE;
	if (ctx->timescale && !ctx->opid) {
		ctx->opid = gf_filter_pid_new(filter);
		gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);
		if (ctx->sei_loader)
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SEI_LOADED, &PROP_BOOL(GF_TRUE) );
	}

	//if source has no timescale, recompute time
	if (!ctx->timescale) {
		ctx->notime = GF_TRUE;
	} else {
		//if we have a FPS prop, use it
		p = gf_filter_pid_get_property(pid, GF_PROP_PID_FPS);
		if (p) ctx->cur_fps = p->value.frac;

		ctx->copy_props = GF_TRUE;
	}
	return GF_OK;
}

GF_Err av1dmx_check_format(GF_Filter *filter, GF_AV1DmxCtx *ctx, GF_BitStream *bs, u32 *last_obu_end)
{
	GF_Err e;
	const GF_PropertyValue *p;
	if (last_obu_end) (*last_obu_end) = 0;
	//probing av1 bs mode
	if (ctx->bsmode != NOT_SET) return GF_OK;


	if (!ctx->state.config)
		ctx->state.config = gf_odf_av1_cfg_new();

	if (!ctx->iamfstate.config) {
		ctx->iamfstate.config = gf_odf_ia_cfg_new();
		if (!ctx->iamfstate.config) return GF_OUT_OF_MEM;
	}

	ctx->is_av1 = ctx->is_vp9 = ctx->is_iamf = GF_FALSE;
	if (ctx->sei_loader) {
		gf_sei_loader_del(ctx->sei_loader);
		ctx->sei_loader = NULL;
	}
	ctx->codecid = 0;
	if (ctx->vp_cfg) gf_odf_vp_cfg_del(ctx->vp_cfg);
	ctx->vp_cfg = NULL;
	ctx->cur_fps = ctx->fps;
	if (!ctx->fps.num || !ctx->fps.den) {
		ctx->cur_fps.num = 25000;
		ctx->cur_fps.den = 1000;
	}

	ctx->pts_from_file = GF_FALSE;
	if (gf_media_probe_iamf(bs)) {
		ctx->bsmode = IAMF;
		ctx->is_iamf = GF_TRUE;
		ctx->codecid = GF_CODECID_IAMF;
		if (last_obu_end) (*last_obu_end) = (u32) gf_bs_get_position(bs);

		return GF_OK;
	}
	if (gf_media_probe_ivf(bs)) {
		u32 width = 0, height = 0;
		u32 codec_fourcc = 0, timebase_den = 0, timebase_num = 0, num_frames = 0;
		ctx->bsmode = IVF;

		e = gf_media_parse_ivf_file_header(bs, &width, &height, &codec_fourcc, &timebase_num, &timebase_den, &num_frames);
		if (e) return e;

		switch (codec_fourcc) {
		case GF_4CC('A', 'V', '0', '1'):
			ctx->is_av1 = GF_TRUE;
			ctx->codecid = GF_CODECID_AV1;
			if (!ctx->sei_loader) ctx->sei_loader = gf_sei_loader_new();
			gf_sei_init_from_av1(ctx->sei_loader, &ctx->state);
			break;
		case GF_4CC('V', 'P', '9', '0'):
			ctx->is_vp9 = GF_TRUE;
			ctx->codecid = GF_CODECID_VP9;
			ctx->vp_cfg = gf_odf_vp_cfg_new();
			break;
		case GF_4CC('V', 'P', '8', '0'):
			ctx->codecid = GF_CODECID_VP8;
			ctx->vp_cfg = gf_odf_vp_cfg_new();
			break;
		case GF_4CC('V', 'P', '1', '0'):
			ctx->codecid = GF_CODECID_VP10;
			ctx->vp_cfg = gf_odf_vp_cfg_new();
			GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[IVF] %s parsing not implemented, import might be uncomplete or broken\n", gf_4cc_to_str(codec_fourcc) ));
			break;
		default:
			ctx->codecid = codec_fourcc;
			GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[IVF] Unsupported codec FourCC %s\n", gf_4cc_to_str(codec_fourcc) ));
			return GF_NON_COMPLIANT_BITSTREAM;
		}
		if (ctx->vp_cfg && !ctx->is_vp9) {
			ctx->vp_cfg->profile = 1;
			ctx->vp_cfg->level = 10;
			ctx->vp_cfg->bit_depth = 8;
			//leave the rest as 0
		}

		ctx->state.width = ctx->state.width < width ? width : ctx->state.width;
		ctx->state.height = ctx->state.height < height ? height : ctx->state.height;
		ctx->state.tb_num = timebase_num;
		ctx->state.tb_den = timebase_den;
		ctx->num_frames = num_frames;

		if ((!ctx->fps.num || !ctx->fps.den) && ctx->state.tb_num && ctx->state.tb_den && ! ( (ctx->state.tb_num<=1) && (ctx->state.tb_den<=1) ) ) {
			ctx->cur_fps.num = ctx->state.tb_num;
			ctx->cur_fps.den = ctx->state.tb_den;
			GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[AV1Dmx] Detected IVF format FPS %d/%d\n", ctx->cur_fps.num, ctx->cur_fps.den));
			ctx->pts_from_file = GF_TRUE;
		} else {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[AV1Dmx] Detected IVF format\n"));
		}
		ctx->file_hdr_size = (u32) gf_bs_get_position(bs);
		if (last_obu_end) (*last_obu_end) = (u32) gf_bs_get_position(bs);
		return GF_OK;
	}

	ctx->codecid = 0;
	p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_CODECID);
	if (p && (p->value.uint!=GF_CODECID_AV1)) {
		switch (p->value.uint) {
		case GF_CODECID_VP9:
			ctx->is_vp9 = GF_TRUE;
		case GF_CODECID_VP8:
		case GF_CODECID_VP10:
			ctx->vp_cfg = gf_odf_vp_cfg_new();
			ctx->codecid = p->value.uint;
			if (ctx->vp_cfg && !ctx->is_vp9) {
				ctx->vp_cfg->profile = 1;
				ctx->vp_cfg->level = 10;
				ctx->vp_cfg->bit_depth = 8;
				//leave the rest as 0
			}
			break;
		}
	}

	if (ctx->codecid) {
		ctx->bsmode = RAW_VPX;
		p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_WIDTH);
		if (p) ctx->state.width = p->value.uint;
		p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_HEIGHT);
		if (p) ctx->state.height = p->value.uint;
		return GF_OK;
	}


	if (gf_media_aom_probe_annexb(bs)) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[AV1Dmx] Detected Annex B format\n"));
		ctx->bsmode = AnnexB;
	} else {
		gf_bs_seek(bs, 0);
		e = aom_av1_parse_temporal_unit_from_section5(bs, &ctx->state);
		if (e && !gf_list_count(ctx->state.frame_state.frame_obus) ) {
			if (e==GF_BUFFER_TOO_SMALL) {
				gf_av1_reset_state(&ctx->state, GF_FALSE);
				return GF_BUFFER_TOO_SMALL;
			}
			gf_filter_setup_failure(filter, e);
			ctx->bsmode = UNSUPPORTED;
			return e;
		}
		if (!ctx->timescale && !ctx->state.has_temporal_delim) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[AV1Dmx] Error OBU stream start with %s, not a temporal delimiter\n", gf_av1_get_obu_name(ctx->state.obu_type) ));
			if (!e) e = GF_NON_COMPLIANT_BITSTREAM;
			gf_filter_setup_failure(filter, e);
			ctx->bsmode = UNSUPPORTED;
			return e;
		}
		GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[AV1Dmx] Detected OBUs Section 5 format\n"));
		ctx->bsmode = OBUs;

		gf_av1_reset_state(&ctx->state, GF_FALSE);
		gf_bs_seek(bs, 0);
	}
	ctx->is_av1 = GF_TRUE;
	ctx->state.unframed = GF_TRUE;
	ctx->codecid = GF_CODECID_AV1;
	if (!ctx->sei_loader) ctx->sei_loader = gf_sei_loader_new();
	gf_sei_init_from_av1(ctx->sei_loader, &ctx->state);
	return GF_OK;
}

GF_Err gf_bs_set_logger(GF_BitStream *bs, void (*on_bs_log)(void *udta, const char *field_name, u32 nb_bits, u64 field_val, s32 idx1, s32 idx2, s32 idx3), void *udta);
static void av1dmx_bs_log(void *udta, const char *field_name, u32 nb_bits, u64 field_val, s32 idx1, s32 idx2, s32 idx3)
{
	GF_AV1DmxCtx *ctx = (GF_AV1DmxCtx *) udta;
	GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, (" %s", field_name));
	if (idx1>=0) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("_%d", idx1));
		if (idx2>=0) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("_%d", idx2));
			if (idx3>=0) {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("_%d", idx3));
			}
		}
	}
	GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("=\""LLD, field_val));
	if ((ctx->bsdbg==2) && ((s32) nb_bits > 1) )
		GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("(%u)", nb_bits));

	GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("\" "));
}

static void av1dmx_check_dur(GF_Filter *filter, GF_AV1DmxCtx *ctx)
{
	FILE *stream;
	GF_Err e;
	GF_BitStream *bs;
	u64 duration, cur_dur, last_cdur, file_size, max_pts, last_pts, probe_size=0;
	AV1State *av1state=NULL;
	IAMFState *iamfstate=NULL;
	const char *filepath=NULL;
	const GF_PropertyValue *p;
	if (!ctx->opid || ctx->timescale || ctx->file_loaded) return;

	p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILE_CACHED);
	if (p && p->value.boolean) ctx->file_loaded = GF_TRUE;

	p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILEPATH);
	if (!p || !p->value.string || !strncmp(p->value.string, "gmem://", 7)) {
		ctx->is_file = GF_FALSE;
		ctx->file_loaded = GF_TRUE;
		return;
	}
	filepath = p->value.string;
	ctx->is_file = GF_TRUE;

	if (ctx->index<0) {
		if (gf_opts_get_bool("temp", "force_indexing")) {
			ctx->index = 1.0;
		} else {
			p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_DOWN_SIZE);
			if (!p || (p->value.longuint > 20000000)) {
				GF_LOG(GF_LOG_INFO, GF_LOG_MEDIA, ("[AV1/VP9/IAMF] Source file larger than 20M, skipping indexing\n"));
				if (!gf_sys_is_test_mode())
					probe_size = 20000000;
			} else {
				ctx->index = -ctx->index;
			}
		}
	}
	if ((ctx->index<=0 )&& !probe_size)
		return;

	stream = gf_fopen_ex(filepath, NULL, "rb", GF_TRUE);
	if (!stream) {
		if (gf_fileio_is_main_thread(p->value.string))
			ctx->file_loaded = GF_TRUE;
		return;
	}

	ctx->index_size = 0;
	switch (ctx->bsmode) {
	case IAMF:
		GF_SAFEALLOC(iamfstate, IAMFState);
		if (!iamfstate) {
			return;
		}
		break;
	default:
		GF_SAFEALLOC(av1state, AV1State);
		if (!av1state) {
			return;
		}
	}

	bs = gf_bs_from_file(stream, GF_BITSTREAM_READ);
#ifndef GPAC_DISABLE_LOG
	if (ctx->bsdbg && gf_log_tool_level_on(GF_LOG_MEDIA, GF_LOG_DEBUG))
		gf_bs_set_logger(bs, av1dmx_bs_log, ctx);
#endif

	if (ctx->file_hdr_size) {
		gf_bs_seek(bs, ctx->file_hdr_size);
	}
	file_size = gf_bs_available(bs);

	switch (ctx->bsmode) {
	case IAMF:
		gf_iamf_init_state(iamfstate);
		iamfstate->config = gf_odf_ia_cfg_new();
		if (!iamfstate->config) return;
		break;
	default:
		gf_av1_init_state(av1state);
		av1state->skip_frames = GF_TRUE;
		av1state->config = gf_odf_av1_cfg_new();
	}

	max_pts = last_pts = 0;
	duration = 0;
	cur_dur = last_cdur = 0;
	while (gf_bs_available(bs)) {
		Bool is_sap=GF_FALSE;
		u64 pts = GF_FILTER_NO_TS;
		u64 frame_start = gf_bs_get_position(bs);

		if (probe_size && (frame_start>probe_size))
			break;

		switch (ctx->bsmode) {
		case IAMF:
			gf_iamf_reset_state(iamfstate, GF_FALSE);
			break;
		default:
			gf_av1_reset_state(av1state, GF_FALSE);
		}

		/*we process each TU and extract only the necessary OBUs*/
		switch (ctx->bsmode) {
		case OBUs:
			e = aom_av1_parse_temporal_unit_from_section5(bs, av1state);
			break;
		case AnnexB:
			e = aom_av1_parse_temporal_unit_from_annexb(bs, av1state);
			break;
		case IVF:
			if (ctx->is_av1) {
				e = aom_av1_parse_temporal_unit_from_ivf(bs, av1state);
			} else {
				u64 frame_size;
				e = gf_media_parse_ivf_frame_header(bs, &frame_size, &pts);
				if (!e) gf_bs_skip_bytes(bs, frame_size);
		 		is_sap = GF_TRUE;
		 		pts *= ctx->cur_fps.den;
			}
			break;
		case IAMF:
			e = aom_iamf_parse_temporal_unit(bs, iamfstate);
			is_sap = GF_TRUE;
			break;
		default:
			e = GF_NOT_SUPPORTED;
		}
		if (e)
		 	break;

		if (pts != GF_FILTER_NO_TS) {
			if (pts + max_pts < last_pts) {
				max_pts = last_pts + ctx->cur_fps.den;
			}
			pts += max_pts;
			duration = pts;
			cur_dur = pts - last_cdur;

			last_pts = pts;
		} else {
			duration += ctx->cur_fps.den;
			cur_dur += ctx->cur_fps.den;
		}
		if (ctx->bsmode != IAMF && av1state->frame_state.key_frame)
		 	is_sap = GF_TRUE;

		//only index at I-frame start
		if (!probe_size && frame_start && is_sap && (cur_dur > ctx->index * ctx->cur_fps.num) ) {
			if (!ctx->index_alloc_size) ctx->index_alloc_size = 10;
			else if (ctx->index_alloc_size == ctx->index_size) ctx->index_alloc_size *= 2;
			ctx->indexes = gf_realloc(ctx->indexes, sizeof(AV1Idx)*ctx->index_alloc_size);
			ctx->indexes[ctx->index_size].pos = frame_start;
			ctx->indexes[ctx->index_size].duration = (Double) duration;
			ctx->indexes[ctx->index_size].duration /= ctx->cur_fps.num;
			ctx->index_size ++;
			last_cdur = cur_dur;
			cur_dur = 0;
		}
	}
	if (probe_size)
		probe_size = gf_bs_get_position(bs);
	gf_bs_del(bs);
	gf_fclose(stream);
	switch (ctx->bsmode) {
	case IAMF:
		if (iamfstate->config) gf_odf_ia_cfg_del(iamfstate->config);
		gf_iamf_reset_state(iamfstate, GF_TRUE);
		gf_free(iamfstate);
		break;
	default:
		if (av1state->config) gf_odf_av1_cfg_del(av1state->config);
		gf_av1_reset_state(av1state, GF_TRUE);
		gf_free(av1state);
	}

	if (!ctx->duration.num || (ctx->duration.num  * ctx->cur_fps.num != duration * ctx->duration.den)) {
		if (probe_size) {
			duration *= file_size / probe_size;
		}
		ctx->duration.num = (s32) duration;
		ctx->duration.den = ctx->cur_fps.num;

		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DURATION, & PROP_FRAC64(ctx->duration));
		if (probe_size)
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DURATION_AVG, &PROP_BOOL(GF_TRUE) );

		if (ctx->duration.num && (!gf_sys_is_test_mode() || gf_opts_get_bool("temp", "force_indexing"))) {
			file_size *= 8 * ctx->duration.den;
			file_size /= ctx->duration.num;
			ctx->bitrate = (u32) file_size;
		}
	}
}


static Bool av1dmx_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	u32 i;
	u64 file_pos = 0;
	GF_FilterEvent fevt;
	GF_AV1DmxCtx *ctx = gf_filter_get_udta(filter);

	switch (evt->base.type) {
	case GF_FEVT_PLAY:
		if (!ctx->is_playing) {
			ctx->is_playing = GF_TRUE;
			ctx->cts = 0;
		}
		if (! ctx->is_file) {
			return GF_FALSE;
		}
		ctx->start_range = evt->play.start_range;
		ctx->in_seek = GF_TRUE;

		if (ctx->start_range) {

			if (ctx->index<0) {
				ctx->index = -ctx->index;
				ctx->file_loaded = GF_FALSE;
				ctx->duration.den = ctx->duration.num = 0;
				GF_LOG(GF_LOG_INFO, GF_LOG_MEDIA, ("[AV1/VP9Demx] Play request from %d, building index\n", ctx->start_range));
				av1dmx_check_dur(filter, ctx);
			}

			for (i=1; i<ctx->index_size; i++) {
				if (ctx->indexes[i].duration>ctx->start_range) {
					ctx->cts = (u64) (ctx->indexes[i-1].duration * ctx->cur_fps.num);
					file_pos = ctx->indexes[i-1].pos;
					break;
				}
			}
		}
		if (!ctx->initial_play_done) {
			ctx->initial_play_done = GF_TRUE;
			//seek will not change the current source state, don't send a seek
			if (!file_pos)
				return GF_TRUE;
		}
		ctx->buf_size = 0;
		if (!file_pos)
			file_pos = ctx->file_hdr_size;

		//post a seek
		GF_FEVT_INIT(fevt, GF_FEVT_SOURCE_SEEK, ctx->ipid);
		fevt.seek.start_offset = file_pos;
		gf_filter_pid_send_event(ctx->ipid, &fevt);

		//cancel event
		return GF_TRUE;

	case GF_FEVT_STOP:
		//don't cancel event
		ctx->is_playing = GF_FALSE;
		ctx->cts = 0;
		ctx->buf_size = 0;
		return GF_FALSE;

	case GF_FEVT_SET_SPEED:
		//cancel event
		return GF_TRUE;
	default:
		break;
	}
	//by default don't cancel event - to rework once we have downloading in place
	return GF_FALSE;
}

static GFINLINE void av1dmx_update_cts(GF_AV1DmxCtx *ctx)
{
	gf_assert(ctx->cur_fps.num);
	gf_assert(ctx->cur_fps.den);

	if (!ctx->notime) {
		u64 inc = ctx->cur_fps.den;
		inc *= ctx->timescale;
		inc /= ctx->cur_fps.num;
		ctx->cts += inc;
	} else {
		ctx->cts += ctx->cur_fps.den;
	}
}

static void av1dmx_check_pid(GF_Filter *filter, GF_AV1DmxCtx *ctx)
{
	u8 *dsi;
	u32 dsi_size, crc;

	//no config or no config change
	if (ctx->is_av1 && !gf_list_count(ctx->state.frame_state.header_obus)) return;
	if (ctx->is_iamf && (!ctx->iamfstate.frame_state.pre_skip_is_finalized || !gf_list_count(ctx->iamfstate.frame_state.descriptor_obus))) return;

	if (ctx->is_iamf) {
		ctx->cur_fps.num = ctx->iamfstate.sample_rate;
		ctx->cur_fps.den = ctx->iamfstate.num_samples_per_frame;
	}

	if (!ctx->opid) {
		if (ctx->bsmode==UNSUPPORTED) return;
		ctx->opid = gf_filter_pid_new(filter);
		av1dmx_check_dur(filter, ctx);
	}
	dsi = NULL;
	dsi_size = 0;

	crc = 0;
	if (ctx->vp_cfg) {
		gf_odf_vp_cfg_write(ctx->vp_cfg, &dsi, &dsi_size, ctx->vp_cfg->codec_initdata_size ? GF_TRUE : GF_FALSE);
		crc = gf_crc_32(dsi, dsi_size);
	} else if (ctx->is_av1) {
		//first or config changed, compute dsi
		while (gf_list_count(ctx->state.config->obu_array)) {
			GF_AV1_OBUArrayEntry *a = (GF_AV1_OBUArrayEntry*) gf_list_pop_back(ctx->state.config->obu_array);
			if (a->obu) gf_free(a->obu);
			gf_free(a);
		}
		dsi = NULL;
		dsi_size = 0;
		while (gf_list_count(ctx->state.frame_state.header_obus)) {
			GF_AV1_OBUArrayEntry *a = (GF_AV1_OBUArrayEntry*) gf_list_get(ctx->state.frame_state.header_obus, 0);
			gf_list_add(ctx->state.config->obu_array, a);
			gf_list_rem(ctx->state.frame_state.header_obus, 0);
			if (a->obu_type == OBU_SEQUENCE_HEADER) {
				crc = gf_crc_32(a->obu, (u32) a->obu_length);
			}
		}
		gf_odf_av1_cfg_write(ctx->state.config, &dsi, &dsi_size);

		if ((!ctx->fps.num || !ctx->fps.den) && ctx->state.tb_num && ctx->state.tb_den && ! ( (ctx->state.tb_num<=1) && (ctx->state.tb_den<=1) ) ) {
			ctx->cur_fps.num = ctx->state.tb_num;
			ctx->cur_fps.den = ctx->state.tb_den;
		}
		if (!crc) {
			gf_free(dsi);
			return;
		}
	} else if (ctx->is_iamf) {
		//IAMF Descriptors changed, compute dsi

		//Clear any old configOBUs - these will be repopulated from the descriptor OBUs below.
		while (gf_list_count(ctx->iamfstate.config->configOBUs)) {
			GF_IamfObu *a = (GF_IamfObu*) gf_list_pop_back(ctx->iamfstate.config->configOBUs);
			if (a->raw_obu_bytes) gf_free(a->raw_obu_bytes);
			gf_free(a);
		}
		ctx->iamfstate.config->configOBUs_size = 0;

		dsi = NULL;
		dsi_size = 0;

		while (gf_list_count(ctx->iamfstate.frame_state.descriptor_obus)) {
			GF_IamfObu *a = (GF_IamfObu*) gf_list_get(ctx->iamfstate.frame_state.descriptor_obus, 0);
			gf_list_add(ctx->iamfstate.config->configOBUs, a);
			ctx->iamfstate.config->configOBUs_size += (u32) a->obu_length;
			gf_list_rem(ctx->iamfstate.frame_state.descriptor_obus, 0);
		}

		gf_odf_ia_cfg_write(ctx->iamfstate.config, &dsi, &dsi_size);

		// Compute the CRC of the entire iacb box.
		crc = gf_crc_32(dsi, (u32) dsi_size);
	}

	if ((crc == ctx->dsi_crc) && !ctx->copy_props) {
		gf_free(dsi);
		return;
	}
	ctx->dsi_crc = crc;

	//copy properties at init or reconfig
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	ctx->copy_props = GF_FALSE;

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);

	if (ctx->is_iamf) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, & PROP_UINT(GF_STREAM_AUDIO));
		if(ctx->iamfstate.pre_skip > 0) {
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DELAY,  &PROP_LONGSINT(- (s64)ctx->iamfstate.pre_skip));
		}
	} else {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, & PROP_UINT(GF_STREAM_VISUAL));
	}
	if (ctx->sei_loader)
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SEI_LOADED, &PROP_BOOL(GF_TRUE) );

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, & PROP_UINT(ctx->codecid));
	if (!ctx->timescale) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, & PROP_UINT(ctx->cur_fps.num));
	}

	//if we have a FPS prop, use it
	if (!gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FPS))
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS, & PROP_FRAC(ctx->cur_fps));

	if (ctx->state.sequence_width && ctx->state.sequence_height) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, & PROP_UINT(ctx->state.sequence_width));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, & PROP_UINT(ctx->state.sequence_height));
	} else {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, & PROP_UINT(ctx->state.width));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, & PROP_UINT(ctx->state.height));
	}

	if (ctx->duration.num)
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DURATION, & PROP_FRAC64(ctx->duration));

	//currently not supported because of OBU size field rewrite - could work on some streams but we would
	//need to analyse all OBUs in the stream for that
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CAN_DATAREF, NULL );

	if (ctx->bitrate) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_BITRATE, & PROP_UINT(ctx->bitrate));
	}

	if (dsi && dsi_size)
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, & PROP_DATA_NO_COPY(dsi, dsi_size));

	if (ctx->is_file && ctx->index) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PLAYBACK_MODE, & PROP_UINT(GF_PLAYBACK_MODE_FASTFORWARD) );
	}
	if (ctx->num_frames) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NB_FRAMES, & PROP_UINT(ctx->num_frames) );
	}

	ctx->clli_crc = 0;
	ctx->mdcv_crc = 0;
	if (ctx->is_av1) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_PRIMARIES, & PROP_UINT(ctx->state.color_primaries) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_TRANSFER, & PROP_UINT(ctx->state.transfer_characteristics) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_MX, & PROP_UINT(ctx->state.matrix_coefficients) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_RANGE, & PROP_BOOL(ctx->state.color_range) );


		if (ctx->state.sei.clli_valid) {
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CONTENT_LIGHT_LEVEL, &PROP_DATA(ctx->state.sei.clli_data, 4));
			ctx->clli_crc = gf_crc_32(ctx->state.sei.clli_data, 4);
			ctx->state.sei.clli_valid = 0;
		}
		if (ctx->state.sei.mdcv_valid) {
			u8 rw_mdcv[24];
			gf_av1_format_mdcv_to_mpeg(ctx->state.sei.mdcv_data, rw_mdcv);
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_MASTER_DISPLAY_COLOUR, &PROP_DATA(rw_mdcv, 24));
			ctx->mdcv_crc = gf_crc_32(ctx->state.sei.mdcv_data, 24);
			ctx->state.sei.mdcv_valid = 0;
		}

	}
	//disabled for the time being, matchin `colr` box will be injected by mp43mx if needed
	//check vpX specs to see if always needed
#if 0
	else if (ctx->vp_cfg) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_PRIMARIES, & PROP_UINT(ctx->vp_cfg->colour_primaries) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_TRANSFER, & PROP_UINT(ctx->vp_cfg->transfer_characteristics) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_MX, & PROP_UINT(ctx->vp_cfg->matrix_coefficients) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_COLR_RANGE, & PROP_BOOL(ctx->vp_cfg->video_fullRange_flag) );
	}
#endif
}

GF_Err av1dmx_parse_ivf(GF_Filter *filter, GF_AV1DmxCtx *ctx)
{
	GF_Err e;
	u32 pck_size;
	u64 frame_size = 0, pts = GF_FILTER_NO_TS;
	GF_FilterPacket *pck;
	u64 pos=0, pos_ivf_hdr=0;
	u8 *output=NULL;

	if (ctx->bsmode==IVF) {
		pos_ivf_hdr = gf_bs_get_position(ctx->bs);
		e = gf_media_parse_ivf_frame_header(ctx->bs, &frame_size, &pts);
		if (e) return e;

		pos = gf_bs_get_position(ctx->bs);
		if (gf_bs_available(ctx->bs) < frame_size) {
			gf_bs_seek(ctx->bs, pos_ivf_hdr);
			return GF_EOS;
		}
		if (ctx->pts_from_file) {
			pts *= ctx->cur_fps.den;
			pts += ctx->cumulated_dur;
			if (ctx->last_pts && (ctx->last_pts>pts)) {
				pts -= ctx->cumulated_dur;
				GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[IVF/AV1] Corrupted timestamp "LLU" less than previous timestamp "LLU", assuming concatenation\n", pts, ctx->last_pts));
				ctx->cumulated_dur = ctx->last_pts + ctx->cur_fps.den;
				ctx->cumulated_dur -= pts;
				pts = ctx->cumulated_dur;
			}
			ctx->last_pts = pts;
		}
	} else {
		//raw framed input, each packet is a frame
		pts = ctx->src_pck ? gf_filter_pck_get_cts(ctx->src_pck) : 0;
		pos = 0;
		frame_size = gf_bs_available(ctx->bs);
	}

	//check pid state
	av1dmx_check_pid(filter, ctx);

	if (!ctx->opid) {
		return GF_OK;
	}

	if (!ctx->is_playing) {
		gf_bs_seek(ctx->bs, pos_ivf_hdr);
		return GF_EOS;
	}

	pck_size = (u32)frame_size;
	pck = gf_filter_pck_new_alloc(ctx->opid, pck_size, &output);
	if (!pck) {
		gf_bs_seek(ctx->bs, pos_ivf_hdr);
		return GF_OUT_OF_MEM;
	}
	if (ctx->src_pck) gf_filter_pck_merge_properties(ctx->src_pck, pck);

	if (ctx->pts_from_file) {
		gf_filter_pck_set_cts(pck, pts);
	} else {
		gf_filter_pck_set_cts(pck, ctx->cts);
	}

	gf_bs_seek(ctx->bs, pos);

	if (gf_bs_read_data(ctx->bs, output, pck_size) && (output[0] & 0x80))
		gf_filter_pck_set_sap(pck, GF_FILTER_SAP_1);
	else
		gf_filter_pck_set_sap(pck, GF_FILTER_SAP_NONE);

	gf_filter_pck_send(pck);

	av1dmx_update_cts(ctx);
	return GF_OK;
}

GF_Err av1dmx_parse_vp9(GF_Filter *filter, GF_AV1DmxCtx *ctx)
{
	Bool key_frame = GF_FALSE;
	u64 frame_size = 0, pts = 0;
	u64 pos=0, pos_ivf_hdr=0;
	u32 width = 0, height = 0, renderWidth, renderHeight;
	u32 num_frames_in_superframe = 0, superframe_index_size = 0, i = 0;
	u32 frame_sizes[VP9_MAX_FRAMES_IN_SUPERFRAME];
	u8 *output;
	GF_Err e;

	if (ctx->bsmode==IVF) {
		pos_ivf_hdr = gf_bs_get_position(ctx->bs);
		e = gf_media_parse_ivf_frame_header(ctx->bs, &frame_size, &pts);
		if (e) return e;
		if (!frame_size) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[IVF/VP9] Corrupted frame header !\n"));
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		pos = gf_bs_get_position(ctx->bs);
		if (gf_bs_available(ctx->bs) < frame_size) {
			gf_bs_seek(ctx->bs, pos_ivf_hdr);
			return GF_EOS;
		}

		if (ctx->pts_from_file) {
			pts *= ctx->cur_fps.den;
			pts += ctx->cumulated_dur;
			if (ctx->last_pts && (ctx->last_pts-1>pts)) {
				pts -= ctx->cumulated_dur;
				GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[IVF/VP9] Corrupted timestamp "LLU" less than previous timestamp "LLU", assuming concatenation\n", pts, ctx->last_pts-1));
				ctx->cumulated_dur = ctx->last_pts-1 + ctx->cur_fps.den;
				ctx->cumulated_dur -= pts;
				pts = ctx->cumulated_dur;
			}
			ctx->last_pts = pts+1;
		}
	} else {
		//raw framed input, each packet is a frame
		pts = ctx->src_pck ? gf_filter_pck_get_cts(ctx->src_pck) : 0;
		frame_size = gf_bs_available(ctx->bs);
		pos = 0;
	}

	/*check if it is a superframe*/
	e = gf_vp9_parse_superframe(ctx->bs, frame_size, &num_frames_in_superframe, frame_sizes, &superframe_index_size);
	if (e) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[VP9Dmx] Error parsing superframe structure\n"));
		return e;
	}

	for (i = 0; i < num_frames_in_superframe; ++i) {
		u64 pos2 = gf_bs_get_position(ctx->bs);
		if (gf_vp9_parse_sample(ctx->bs, ctx->vp_cfg, &key_frame, &width, &height, &renderWidth, &renderHeight) != GF_OK) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[VP9Dmx] Error parsing frame\n"));
			return e;
		}
		e = gf_bs_seek(ctx->bs, pos2 + frame_sizes[i]);
		if (e) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[VP9Dmx] Seek bad param (offset "LLU") (1)", pos2 + frame_sizes[i]));
			return e;
		}
	}
	if (gf_bs_get_position(ctx->bs) + superframe_index_size != pos + frame_size) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[VP9Dmx] Inconsistent IVF frame size of "LLU" bytes.\n", frame_size));
		GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("      Detected %d frames (+ %d bytes for the superframe index):\n", num_frames_in_superframe, superframe_index_size));
		for (i = 0; i < num_frames_in_superframe; ++i) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("         superframe %d, size is %u bytes\n", i, frame_sizes[i]));
		}
		GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("\n"));
	}
	e = gf_bs_seek(ctx->bs, pos + frame_size);
	if (e) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[VP9Dmx] Seek bad param (offset "LLU") (2)", pos + frame_size));
		return e;
	}

	u32 pck_size = (u32)(gf_bs_get_position(ctx->bs) - pos);
	gf_fatal_assert(pck_size == frame_size);

	//check pid state
	av1dmx_check_pid(filter, ctx);

	if (!ctx->opid) {
		return GF_OK;
	}

	if (!ctx->is_playing) {
		gf_bs_seek(ctx->bs, pos_ivf_hdr);
		return GF_EOS;
	}

	GF_FilterPacket *pck = gf_filter_pck_new_alloc(ctx->opid, pck_size, &output);
	if (!pck) {
		gf_bs_seek(ctx->bs, pos_ivf_hdr);
		return GF_OUT_OF_MEM;
	}
	if (ctx->src_pck) gf_filter_pck_merge_properties(ctx->src_pck, pck);

	if (ctx->pts_from_file) {
		gf_filter_pck_set_cts(pck, pts);
	} else {
		gf_filter_pck_set_cts(pck, ctx->cts);
	}

	if (key_frame) {
		gf_filter_pck_set_sap(pck, GF_FILTER_SAP_1);
	}

	if (ctx->deps) {
		u8 flags = 0;
		//dependsOn
		flags = (key_frame) ? 2 : 1;
		flags <<= 2;
		//dependedOn
		//flags |= 2;
		flags <<= 2;
		//hasRedundant
		//flags |= ctx->has_redundant ? 1 : 2;
		gf_filter_pck_set_dependency_flags(pck, flags);
	}

	gf_bs_seek(ctx->bs, pos);
	gf_bs_read_data(ctx->bs, output, pck_size);
	gf_filter_pck_send(pck);

	av1dmx_update_cts(ctx);
	return GF_OK;
}

static GF_Err av1dmx_parse_flush_sample(GF_Filter *filter, GF_AV1DmxCtx *ctx)
{
	u32 pck_size = 0;
	GF_FilterPacket *pck = NULL;
	u8 *output = NULL;

	if (!ctx->opid)
		return GF_NON_COMPLIANT_BITSTREAM;

	if (ctx->is_iamf) {
		if (ctx->iamfstate.temporal_unit_obus) {
			gf_free(ctx->iamfstate.temporal_unit_obus);
			ctx->iamfstate.temporal_unit_obus = NULL;
		}
		gf_bs_get_content_no_truncate(ctx->iamfstate.bs, &ctx->iamfstate.temporal_unit_obus, &pck_size, &ctx->iamfstate.temporal_unit_obus_alloc);
	} else if (ctx->state.bs && gf_bs_get_size(ctx->state.bs)) {
		gf_bs_get_content_no_truncate(ctx->state.bs, &ctx->state.frame_obus, &pck_size, &ctx->state.frame_obus_alloc);
	}

	if (!pck_size) {
		if (ctx->is_iamf) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[AV1Dmx] no IAMF OBUs making up a temporal unit, skipping OBUs\n"));
		} else {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[AV1Dmx] no frame OBU, skipping OBU\n"));
		}
		return GF_OK;
	}

	pck = gf_filter_pck_new_alloc(ctx->opid, pck_size, &output);
	if (!pck) return GF_OUT_OF_MEM;

	if (ctx->src_pck)
		gf_filter_pck_merge_properties(ctx->src_pck, pck);

	gf_filter_pck_set_cts(pck, ctx->cts);
	gf_filter_pck_set_sap(pck, ctx->state.frame_state.key_frame ? GF_FILTER_SAP_1 : 0);

	if (ctx->is_iamf) {
		memcpy(output, ctx->iamfstate.temporal_unit_obus, pck_size);
                if (ctx->iamfstate.audio_roll_distance != 0) {
			gf_filter_pck_set_roll_info(pck, ctx->iamfstate.audio_roll_distance);
			gf_filter_pck_set_sap(pck, GF_FILTER_SAP_4);
                }
		if (ctx->iamfstate.frame_state.num_samples_to_trim_at_end > 0) {
			u64 trimmed_duration = ctx->iamfstate.num_samples_per_frame - ctx->iamfstate.frame_state.num_samples_to_trim_at_end;
			gf_filter_pck_set_duration(pck, (u32) trimmed_duration);
		}
	} else {
		memcpy(output, ctx->state.frame_obus, pck_size);
	}

	if (ctx->deps) {
		u8 flags = 0;
		//dependsOn
		flags = ( ctx->state.frame_state.key_frame) ? 2 : 1;
		flags <<= 2;
		//dependedOn
	 	flags |= ctx->state.frame_state.refresh_frame_flags ? 1 : 2;
		flags <<= 2;
		//hasRedundant
	 	//flags |= ctx->has_redundant ? 1 : 2;
	 	gf_filter_pck_set_dependency_flags(pck, flags);
	}

	if (ctx->sei_loader)
		gf_sei_load_from_state(ctx->sei_loader, pck);

	gf_filter_pck_send(pck);

	av1dmx_update_cts(ctx);
	gf_av1_reset_state(&ctx->state, GF_FALSE);
	gf_iamf_reset_state(&ctx->iamfstate, GF_FALSE);

	return GF_OK;

}
GF_Err av1dmx_parse_av1(GF_Filter *filter, GF_AV1DmxCtx *ctx)
{
	GF_Err e = GF_OK;
	u64 start;

	if (!ctx->is_playing) {
		ctx->state.frame_state.is_first_frame = GF_TRUE;
	}

	/*we process each TU and extract only the necessary OBUs*/
	start = gf_bs_get_position(ctx->bs);
	switch (ctx->bsmode) {
	case OBUs:
		//first frame loaded !
		if (ctx->state.bs && gf_bs_get_position(ctx->state.bs) && (ctx->state.obu_type == OBU_TEMPORAL_DELIMITER)) {
			e = GF_OK;
		} else {
			e = aom_av1_parse_temporal_unit_from_section5(ctx->bs, &ctx->state);
		}
		break;
	case AnnexB:
		//first TU loaded !
		if (ctx->state.bs && gf_bs_get_position(ctx->state.bs)) {
			e = GF_OK;
		} else {
			e = aom_av1_parse_temporal_unit_from_annexb(ctx->bs, &ctx->state);
			if (e==GF_BUFFER_TOO_SMALL) {
				gf_av1_reset_state(&ctx->state, GF_FALSE);
				gf_bs_seek(ctx->bs, start);
			}
		}
		break;
	case IVF:
		//first frame loaded !
		if (ctx->state.bs && gf_bs_get_position(ctx->state.bs)) {
			e = GF_OK;
		} else {
			e = aom_av1_parse_temporal_unit_from_ivf(ctx->bs, &ctx->state);
		}
		break;
	default:
		e = GF_NOT_SUPPORTED;
	}

	//check pid state
	av1dmx_check_pid(filter, ctx);

	//fixme, we need to flush at each DFG start - for now we assume one PES = one DFG as we do in the muxer
	if (ctx->timescale && (e==GF_BUFFER_TOO_SMALL))
		e = GF_OK;

	if (e) {
		if (e!=GF_EOS && e!=GF_BUFFER_TOO_SMALL) {
			av1dmx_parse_flush_sample(filter, ctx);
		}
		return e;
	}


	if (!ctx->opid) {
		if (ctx->state.obu_type != OBU_TEMPORAL_DELIMITER) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[AV1Dmx] output pid not configured (no sequence header yet ?), skipping OBU\n"));
		}
		gf_av1_reset_state(&ctx->state, GF_FALSE);
		return GF_OK;
	}

	if (!ctx->is_playing) {
		//don't reset state we would skip seq header obu in first frame
		//gf_av1_reset_state(&ctx->state, GF_FALSE);
		return GF_OK;
	}

	e = av1dmx_parse_flush_sample(filter, ctx);
	ctx->state.sei.clli_valid = ctx->state.sei.mdcv_valid = 0;
	return e;
}

GF_Err av1dmx_parse_iamf(GF_Filter *filter, GF_AV1DmxCtx *ctx)
{
	GF_Err e = GF_OK;

	//first TU loaded !
	u64 start = gf_bs_get_position(ctx->bs);
	if (ctx->iamfstate.frame_state.found_full_temporal_unit) {
		e = GF_OK;
	} else {
		e = aom_iamf_parse_temporal_unit(ctx->bs, &ctx->iamfstate);
		if (e==GF_BUFFER_TOO_SMALL) {
			gf_iamf_reset_state(&ctx->iamfstate, GF_FALSE);
			gf_bs_seek(ctx->bs, start);
		}
	}

	//check pid state
	av1dmx_check_pid(filter, ctx);

	if (e) {
		if (e!=GF_EOS && e!=GF_BUFFER_TOO_SMALL) {
			av1dmx_parse_flush_sample(filter, ctx);
		}
		return e;
	}

	if (!ctx->opid) {
		if (ctx->iamfstate.frame_state.pre_skip_is_finalized) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MEDIA, ("[AV1Dmx] output pid not configured (no IAMF Descriptors yet?), skipping OBUs\n"));
		}
		gf_iamf_reset_state(&ctx->iamfstate, GF_FALSE);
		return GF_OK;
	}

	e = av1dmx_parse_flush_sample(filter, ctx);
	return e;
}

GF_Err av1dmx_process_buffer(GF_Filter *filter, GF_AV1DmxCtx *ctx, const char *data, u32 data_size, Bool is_copy)
{
	u32 last_obu_end = 0;
	GF_Err e = GF_OK;

	if (!ctx->bs) ctx->bs = gf_bs_new(data, data_size, GF_BITSTREAM_READ);
	else gf_bs_reassign_buffer(ctx->bs, data, data_size);

#ifndef GPAC_DISABLE_LOG
	if (ctx->bsdbg && gf_log_tool_level_on(GF_LOG_MEDIA, GF_LOG_DEBUG))
		gf_bs_set_logger(ctx->bs, av1dmx_bs_log, ctx);
#endif

	//check ivf vs obu vs annexB vs iamf
	e = av1dmx_check_format(filter, ctx, ctx->bs, &last_obu_end);
	if (e==GF_BUFFER_TOO_SMALL) return GF_OK;
	else if (e) return e;

	while (gf_bs_available(ctx->bs)) {

		if (ctx->is_iamf) {
			e = av1dmx_parse_iamf(filter, ctx);
		} else if (ctx->is_vp9) {
			e = av1dmx_parse_vp9(filter, ctx);
		} else if (ctx->is_av1) {
			e = av1dmx_parse_av1(filter, ctx);
		} else {
			e = av1dmx_parse_ivf(filter, ctx);
		}

		if (e!=GF_EOS)
			last_obu_end = (u32) gf_bs_get_position(ctx->bs);

		if (e) {
			break;
		}
		if (!ctx->is_playing && ctx->opid)
			break;
	}

	if (is_copy && last_obu_end) {
		gf_fatal_assert(ctx->buf_size>=last_obu_end);
		memmove(ctx->buffer, ctx->buffer+last_obu_end, sizeof(char) * (ctx->buf_size-last_obu_end));
		ctx->buf_size -= last_obu_end;
	}
	if (e==GF_EOS) return GF_OK;
	if (e==GF_BUFFER_TOO_SMALL) return GF_OK;
	return e;
}

GF_Err av1dmx_process(GF_Filter *filter)
{
	GF_Err e;
	GF_AV1DmxCtx *ctx = gf_filter_get_udta(filter);
	GF_FilterPacket *pck;
	char *data;
	u32 pck_size;

	if (ctx->bsmode == UNSUPPORTED) return GF_EOS;

	//always reparse duration
	if (!ctx->duration.num)
		av1dmx_check_dur(filter, ctx);

	if (!ctx->is_playing && ctx->opid)
		return GF_OK;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck) {
		if (gf_filter_pid_is_eos(ctx->ipid)) {
			//flush
			while (ctx->buf_size) {
				u32 buf_size = ctx->buf_size;
				e = av1dmx_process_buffer(filter, ctx, ctx->buffer, ctx->buf_size, GF_TRUE);
				if (e) break;
				if (buf_size == ctx->buf_size) {
					break;
				}
			}
			if (ctx->state.bs && gf_bs_get_position(ctx->state.bs))
				av1dmx_parse_flush_sample(filter, ctx);

			ctx->buf_size = 0;
			if (ctx->opid)
				gf_filter_pid_set_eos(ctx->opid);
			if (ctx->src_pck) gf_filter_pck_unref(ctx->src_pck);
			ctx->src_pck = NULL;
			return GF_EOS;
		}
		return GF_OK;
	}

	if (ctx->opid) {
		if (!ctx->is_playing || gf_filter_pid_would_block(ctx->opid))
			return GF_OK;
	}

	data = (char *) gf_filter_pck_get_data(pck, &pck_size);

	//input pid is muxed - we flushed pending data , update cts unless recomputing all times
	if (ctx->timescale) {
		Bool start, end;

		e = GF_OK;

		gf_filter_pck_get_framing(pck, &start, &end);
		//middle or end of frame, reaggregation
		if (!start) {
			if (ctx->alloc_size < ctx->buf_size + pck_size) {
				ctx->alloc_size = ctx->buf_size + pck_size;
				ctx->buffer = gf_realloc(ctx->buffer, ctx->alloc_size);
			}
			memcpy(ctx->buffer+ctx->buf_size, data, pck_size);
			ctx->buf_size += pck_size;

			//end of frame, process av1
			if (end) {
				e = av1dmx_process_buffer(filter, ctx, ctx->buffer, ctx->buf_size, GF_TRUE);
			}
			ctx->buf_size=0;
			gf_filter_pid_drop_packet(ctx->ipid);
			return e;
		}
		//flush of pending frame (might have lost something)
		if (ctx->buf_size) {
			e = av1dmx_process_buffer(filter, ctx, ctx->buffer, ctx->buf_size, GF_TRUE);
			ctx->buf_size = 0;
			if (e) return e;
		}

		//beginning of a new frame
		if (!ctx->notime) {
			u64 cts = gf_filter_pck_get_cts(pck);
			if (cts != GF_FILTER_NO_TS)
				ctx->cts = cts;
		}
		if (ctx->src_pck) gf_filter_pck_unref(ctx->src_pck);
		ctx->src_pck = pck;
		gf_filter_pck_ref_props(&ctx->src_pck);
		ctx->buf_size = 0;

		if (!end) {
			if (ctx->alloc_size < ctx->buf_size + pck_size) {
				ctx->alloc_size = ctx->buf_size + pck_size;
				ctx->buffer = gf_realloc(ctx->buffer, ctx->alloc_size);
			}
			memcpy(ctx->buffer+ctx->buf_size, data, pck_size);
			ctx->buf_size += pck_size;
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_OK;
		}
		gf_assert(start && end);
		//process
		e = av1dmx_process_buffer(filter, ctx, data, pck_size, GF_FALSE);

		gf_filter_pid_drop_packet(ctx->ipid);
		return e;
	}

	//not from framed stream, copy buffer
	if (ctx->alloc_size < ctx->buf_size + pck_size) {
		ctx->alloc_size = ctx->buf_size + pck_size;
		ctx->buffer = gf_realloc(ctx->buffer, ctx->alloc_size);
	}
	memcpy(ctx->buffer+ctx->buf_size, data, pck_size);
	ctx->buf_size += pck_size;
	e = av1dmx_process_buffer(filter, ctx, ctx->buffer, ctx->buf_size, GF_TRUE);
	gf_filter_pid_drop_packet(ctx->ipid);
	return e;
}

static GF_Err av1dmx_initialize(GF_Filter *filter)
{
	GF_AV1DmxCtx *ctx = gf_filter_get_udta(filter);
	gf_av1_init_state(&ctx->state);
	if (ctx->temporal_delim)
		ctx->state.keep_temporal_delim = GF_TRUE;
	gf_iamf_init_state(&ctx->iamfstate);

	return GF_OK;
}

static void av1dmx_finalize(GF_Filter *filter)
{
	GF_AV1DmxCtx *ctx = gf_filter_get_udta(filter);
	if (ctx->bs) gf_bs_del(ctx->bs);
	if (ctx->indexes) gf_free(ctx->indexes);

	gf_av1_reset_state(&ctx->state, GF_TRUE);
	if (ctx->state.config) gf_odf_av1_cfg_del(ctx->state.config);
	if (ctx->state.bs) gf_bs_del(ctx->state.bs);
	if (ctx->state.frame_obus) gf_free(ctx->state.frame_obus);
	if (ctx->buffer) gf_free(ctx->buffer);

	if (ctx->vp_cfg) gf_odf_vp_cfg_del(ctx->vp_cfg);

	gf_iamf_reset_state(&ctx->iamfstate, GF_TRUE);
	if (ctx->iamfstate.config) gf_odf_ia_cfg_del(ctx->iamfstate.config);
	if (ctx->iamfstate.bs) gf_bs_del(ctx->iamfstate.bs);
	if (ctx->iamfstate.temporal_unit_obus) gf_free(ctx->iamfstate.temporal_unit_obus);
	if (ctx->sei_loader)
		gf_sei_loader_del(ctx->sei_loader);
}

static const char * av1dmx_probe_data(const u8 *data, u32 size, GF_FilterProbeScore *score)
{
	GF_BitStream *bs = gf_bs_new(data, size, GF_BITSTREAM_READ);
	Bool res;
	u32 lt;
	const char *mime = "video/av1";
	lt = gf_log_get_tool_level(GF_LOG_CODING);
	gf_log_set_tool_level(GF_LOG_CODING, GF_LOG_QUIET);

	if (gf_media_probe_iamf(bs)) {
		res = GF_TRUE;
		*score = GF_FPROBE_SUPPORTED;
		mime = "audio/iamf";
	} else if (gf_media_probe_ivf(bs)) {
		res = GF_TRUE;
		*score = GF_FPROBE_SUPPORTED;
		mime = "video/x-ivf";
	} else if (gf_media_aom_probe_annexb(bs)) {
		res = GF_TRUE;
		*score = GF_FPROBE_SUPPORTED;
	} else {
		res = gf_media_aom_probe_annexb(bs);
		if (res) {
			*score = GF_FPROBE_SUPPORTED;
		} else {
			AV1State *av1_state;
			Bool has_seq_header = GF_FALSE;
			GF_Err e;
			u32 nb_units = 0;
			GF_SAFEALLOC(av1_state, AV1State);
			if (!av1_state) return NULL;

			gf_av1_init_state(av1_state);
			av1_state->config = gf_odf_av1_cfg_new();
			while (gf_bs_available(bs)) {
				e = aom_av1_parse_temporal_unit_from_section5(bs, av1_state);
				if ((e==GF_OK) || (nb_units && (e==GF_BUFFER_TOO_SMALL) ) ) {
					if (!nb_units || gf_list_count(av1_state->frame_state.header_obus) || gf_list_count(av1_state->frame_state.frame_obus)) {
						if (gf_list_count(av1_state->frame_state.header_obus)) {
							has_seq_header = GF_TRUE;
						}
						nb_units++;
						if (e==GF_BUFFER_TOO_SMALL)
							nb_units++;
					} else {
						//we got one frame + seq header without errors, assume maybe supported
						if (has_seq_header) {
							res = GF_TRUE;
							*score = GF_FPROBE_MAYBE_SUPPORTED;
						}
						break;
					}
				}
				//very large frame
				else if (!nb_units && (e==GF_BUFFER_TOO_SMALL)) {
					if (gf_list_count(av1_state->frame_state.header_obus) && av1_state->width && av1_state->height) {
						res = GF_TRUE;
						*score = GF_FPROBE_MAYBE_SUPPORTED;
					}
					break;
				} else {
					break;
				}
				gf_av1_reset_state(av1_state, GF_FALSE);
				if (nb_units>2) {
					res = GF_TRUE;
					*score = GF_FPROBE_SUPPORTED;
					break;
				}
			}
			gf_odf_av1_cfg_del(av1_state->config);
			gf_av1_reset_state(av1_state, GF_TRUE);
			gf_free(av1_state);
		}
	}

	gf_log_set_tool_level(GF_LOG_CODING, lt);

	gf_bs_del(bs);
	if (res) return mime;
	return NULL;
}

static const GF_FilterCapability AV1DmxCaps[] =
{
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "ivf|obu|av1b|av1"),
	CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/x-ivf|video/av1"),
	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_CODECID, GF_CODECID_AV1),
	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_CODECID, GF_CODECID_VP8),
	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_CODECID, GF_CODECID_VP9),
	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_CODECID, GF_CODECID_VP10),
	CAP_BOOL(GF_CAPS_OUTPUT_STATIC_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
	{0},
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_AV1),
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_VP8),
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_VP9),
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_VP10),
	CAP_BOOL(GF_CAPS_INPUT, GF_PROP_PID_UNFRAMED, GF_TRUE),
	{0},
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "obu|iamf"),
	CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/iamf"),
	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_CODECID, GF_CODECID_IAMF),
	CAP_BOOL(GF_CAPS_OUTPUT_STATIC_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
	{0},
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_IAMF),
	CAP_BOOL(GF_CAPS_INPUT, GF_PROP_PID_UNFRAMED, GF_TRUE),
};

#define OFFS(_n)	#_n, offsetof(GF_AV1DmxCtx, _n)
static const GF_FilterArgs AV1DmxArgs[] =
{
	{ OFFS(fps), "import frame rate (0 default to FPS from bitstream or 25 Hz)", GF_PROP_FRACTION, "0/1000", NULL, 0},
	{ OFFS(index), "indexing window length. If 0, bitstream is not probed for duration. A negative value skips the indexing if the source file is larger than 20M (slows down importers) unless a play with start range > 0 is issued", GF_PROP_DOUBLE, "-1.0", NULL, 0},

	{ OFFS(importer), "compatibility with old importer", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(deps), "import sample dependency information", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(notime), "ignore input timestamps, rebuild from 0", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(temporal_delim), "keep temporal delimiters in reconstructed frames", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_EXPERT},


	{ OFFS(bsdbg), "debug OBU parsing in `media@debug logs\n"
		"- off: not enabled\n"
		"- on: enabled\n"
		"- full: enable with number of bits dumped", GF_PROP_UINT, "off", "off|on|full", GF_FS_ARG_HINT_EXPERT},
	{0}
};


GF_FilterRegister AV1DmxRegister = {
	.name = "rfav1",
	GF_FS_SET_DESCRIPTION("AV1/IVF/VP9/IAMF reframer")
	GF_FS_SET_HELP("This filter parses AV1 OBU, AV1 AnnexB or IVF with AV1 or VP9 files/data and outputs corresponding visual PID and frames. "
		       "It also parses IAMF OBU and outputs corresponding temporal units containing audio frames and parameter blocks.")
	.private_size = sizeof(GF_AV1DmxCtx),
	.args = AV1DmxArgs,
	.initialize = av1dmx_initialize,
	.finalize = av1dmx_finalize,
	SETCAPS(AV1DmxCaps),
	.configure_pid = av1dmx_configure_pid,
	.process = av1dmx_process,
	.probe_data = av1dmx_probe_data,
	.process_event = av1dmx_process_event,
	.hint_class_type = GF_FS_CLASS_FRAMING
};


const GF_FilterRegister *rfav1_register(GF_FilterSession *session)
{
	return &AV1DmxRegister;
}
#else
const GF_FilterRegister *rfav1_register(GF_FilterSession *session)
{
	return NULL;
}
#endif // #if !defined(GPAC_DISABLE_AV_PARSERS) && !defined(GPAC_DISABLE_RFAV1)
