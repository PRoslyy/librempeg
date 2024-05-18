/*
 * Copyright (c) 2023 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <float.h>

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/tx.h"
#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"

typedef struct AudioCenterCutContext {
    const AVClass *class;

    double factor;

    int fft_size;
    int overlap;

    void *window;

    AVFrame *in;
    AVFrame *in_frame;
    AVFrame *out_dist_frame;
    AVFrame *windowed_frame;
    AVFrame *windowed_out;

    int (*cc_stereo)(AVFilterContext *ctx, AVFrame *out);

    AVTXContext *tx_ctx, *itx_ctx;
    av_tx_fn tx_fn, itx_fn;
} AudioCenterCutContext;

#define OFFSET(x) offsetof(AudioCenterCutContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption acentercut_options[] = {
    { "factor", "set the center cut factor", OFFSET(factor), AV_OPT_TYPE_DOUBLE, {.dbl=1}, 0, 1, FLAGS },
    {NULL}
};

AVFILTER_DEFINE_CLASS(acentercut);

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layout = NULL;
    int ret;

    if ((ret = ff_add_format                 (&formats, AV_SAMPLE_FMT_FLTP )) < 0 ||
        (ret = ff_add_format                 (&formats, AV_SAMPLE_FMT_DBLP )) < 0 ||
        (ret = ff_set_common_formats         (ctx     , formats            )) < 0 ||
        (ret = ff_add_channel_layout         (&layout , &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO)) < 0 ||
        (ret = ff_set_common_channel_layouts (ctx     , layout             )) < 0)
        return ret;

    return ff_set_common_all_samplerates(ctx);
}

#define DEPTH 32
#include "acentercut_template.c"

#undef DEPTH
#define DEPTH 64
#include "acentercut_template.c"

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AudioCenterCutContext *s = ctx->priv;
    int ret;

    s->fft_size = inlink->sample_rate > 100000 ? 8192 : inlink->sample_rate > 50000 ? 4096 : 2048;
    s->overlap = s->fft_size / 4;

    s->in_frame       = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    s->out_dist_frame = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    s->windowed_frame = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    s->windowed_out   = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    if (!s->in_frame || !s->windowed_out || !s->out_dist_frame || !s->windowed_frame)
        return AVERROR(ENOMEM);

    switch (inlink->format) {
    case AV_SAMPLE_FMT_FLTP:
        s->cc_stereo = cc_stereo_float;
        ret = cc_tx_init_float(ctx);
        break;
    case AV_SAMPLE_FMT_DBLP:
        s->cc_stereo = cc_stereo_double;
        ret = cc_tx_init_double(ctx);
        break;
    }

    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AudioCenterCutContext *s = ctx->priv;
    AVFrame *out;
    int ret;

    out = ff_get_audio_buffer(outlink, s->overlap);
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    s->in = in;
    s->cc_stereo(ctx, out);

    av_frame_copy_props(out, in);
    out->pts -= av_rescale_q(s->fft_size - s->overlap, av_make_q(1, outlink->sample_rate), outlink->time_base);
    out->nb_samples = in->nb_samples;
    ret = ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    s->in = NULL;
    return ret < 0 ? ret : 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AudioCenterCutContext *s = ctx->priv;
    AVFrame *in = NULL;
    int ret = 0, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = ff_inlink_consume_samples(inlink, s->overlap, s->overlap, &in);
    if (ret < 0)
        return ret;

    if (ret > 0) {
        return filter_frame(inlink, in);
    } else if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    } else {
        if (ff_inlink_queued_samples(inlink) >= s->overlap) {
            ff_filter_set_ready(ctx, 10);
        } else if (ff_outlink_frame_wanted(outlink)) {
            ff_inlink_request_frame(inlink);
        }
        return 0;
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioCenterCutContext *s = ctx->priv;

    av_freep(&s->window);

    av_frame_free(&s->in_frame);
    av_frame_free(&s->out_dist_frame);
    av_frame_free(&s->windowed_frame);
    av_frame_free(&s->windowed_out);

    av_tx_uninit(&s->tx_ctx);
    av_tx_uninit(&s->itx_ctx);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
    },
};

const AVFilter ff_af_acentercut = {
    .name            = "acentercut",
    .description     = NULL_IF_CONFIG_SMALL("Audio Center Cut."),
    .priv_size       = sizeof(AudioCenterCutContext),
    .priv_class      = &acentercut_class,
    .uninit          = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_QUERY_FUNC(query_formats),
    .flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .activate        = activate,
    .process_command = ff_filter_process_command,
};
