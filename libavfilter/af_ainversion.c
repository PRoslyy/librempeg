/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <float.h>
#include <math.h>
#include <stdio.h>

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "audio.h"
#include "avfilter.h"
#include "formats.h"

typedef struct AudioInversionContext {
    const AVClass *class;

    double unity, maxf;
    AVChannelLayout ch_layout;

    int (*filter_channels)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);
} AudioInversionContext;

#define OFFSET(x) offsetof(AudioInversionContext,x)
#define AFT AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption ainversion_options[] = {
    { "unity", "set the unity amplitude", OFFSET(unity), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0, INT16_MAX, AFT },
    { "max", "set the max output", OFFSET(maxf), AV_OPT_TYPE_DOUBLE, {.dbl=5.0}, 1.0, INT16_MAX, AFT },
    { "channels", "set channels to filter", OFFSET(ch_layout), AV_OPT_TYPE_CHLAYOUT, {.str="24c"}, 0, 0, AFT },
    {NULL}
};

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

#define DEPTH 32
#include "ainversion_template.c"

#undef DEPTH
#define DEPTH 64
#include "ainversion_template.c"

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioInversionContext *s = ctx->priv;

    switch (outlink->format) {
    case AV_SAMPLE_FMT_FLTP:
        s->filter_channels = filter_channels_fltp;
        break;
    case AV_SAMPLE_FMT_DBLP:
        s->filter_channels = filter_channels_dblp;
        break;
    default:
        return AVERROR_BUG;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AudioInversionContext *s = ctx->priv;
    ThreadData td;
    AVFrame *out;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_audio_buffer(outlink, in->nb_samples);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    td.in = in;
    td.out = out;
    ff_filter_execute(ctx, s->filter_channels, &td, NULL,
                      FFMIN(outlink->ch_layout.nb_channels,
                            ff_filter_get_nb_threads(ctx)));

    if (in != out)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_output,
    },
};

AVFILTER_DEFINE_CLASS(ainversion);

const FFFilter ff_af_ainversion = {
    .p.name          = "ainversion",
    .p.description   = NULL_IF_CONFIG_SMALL("Apply Audio Inversion Filter."),
    .p.priv_class    = &ainversion_class,
    .priv_size       = sizeof(AudioInversionContext),
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_DBLP),
    .process_command = ff_filter_process_command,
    .p.flags         = AVFILTER_FLAG_SLICE_THREADS |
                       AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
