/*
 * Copyright (c) 2013 Paul B Mahol
 *
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

#include "libavutil/avassert.h"
#include "libavutil/intmath.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"

typedef struct AudioEchoContext {
    const AVClass *class;
    float in_gain, out_gain;
    float *delays;
    unsigned nb_delays;
    float *decays;
    unsigned nb_decays;
    unsigned nb_echoes;
    int *delay_index;
    uint8_t **delayptrs;
    int max_samples, fade_out;
    int *samples;
    int eof;
    int64_t next_pts;

    int (*echo_samples)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);
} AudioEchoContext;

#define OFFSET(x) offsetof(AudioEchoContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define AR AV_OPT_TYPE_FLAG_ARRAY
static const AVOptionArrayDef def_delays = {.def="1000",.size_min=1,.sep='|'};
static const AVOptionArrayDef def_decays = {.def="0.5",.size_min=1,.sep='|'};

static const AVOption aecho_options[] = {
    { "in_gain",  "set signal input gain",  OFFSET(in_gain),  AV_OPT_TYPE_FLOAT,  {.dbl=0.6}, 0, 1, A },
    { "out_gain", "set signal output gain", OFFSET(out_gain), AV_OPT_TYPE_FLOAT,  {.dbl=0.3}, 0, 1, A },
    { "delays",   "set list of signal delays", OFFSET(delays), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_delays}, 0, 90000, A },
    { "decays",   "set list of signal decays", OFFSET(decays), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_decays}, 0, 1, A },
    { NULL }
};

AVFILTER_DEFINE_CLASS(aecho);

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioEchoContext *s = ctx->priv;

    av_freep(&s->delay_index);
    av_freep(&s->samples);

    if (s->delayptrs)
        av_freep(&s->delayptrs[0]);
    av_freep(&s->delayptrs);
}

static av_cold int init(AVFilterContext *ctx)
{
    AudioEchoContext *s = ctx->priv;

    s->nb_echoes = FFMAX(s->nb_delays, s->nb_decays);
    s->samples = av_realloc_f(s->samples, s->nb_echoes, sizeof(*s->samples));
    if (!s->samples)
        return AVERROR(ENOMEM);
    s->next_pts = AV_NOPTS_VALUE;

    av_log(ctx, AV_LOG_DEBUG, "nb_echoes:%d\n", s->nb_echoes);
    return 0;
}

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

#define DEPTH 16
#include "aecho_template.c"

#undef DEPTH
#define DEPTH 31
#include "aecho_template.c"

#undef DEPTH
#define DEPTH 32
#include "aecho_template.c"

#undef DEPTH
#define DEPTH 64
#include "aecho_template.c"

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioEchoContext *s = ctx->priv;
    float volume = 1.0;

    for (int i = 0; i < s->nb_echoes; i++) {
        const unsigned didx = FFMIN(i, s->nb_delays-1);
        const unsigned cidx = FFMIN(i, s->nb_decays-1);
        s->samples[i] = lrintf(s->delays[didx] * outlink->sample_rate / 1000.0);
        s->max_samples = FFMAX(s->max_samples, s->samples[i]);
        volume += s->decays[cidx];
    }

    if (s->max_samples <= 0) {
        av_log(ctx, AV_LOG_ERROR, "Nothing to echo - missing delay samples.\n");
        return AVERROR(EINVAL);
    }
    s->fade_out = s->max_samples;

    if (volume * s->in_gain * s->out_gain > 1.0)
        av_log(ctx, AV_LOG_WARNING,
               "out_gain %f can cause saturation of output\n", s->out_gain);

    switch (outlink->format) {
    case AV_SAMPLE_FMT_DBLP: s->echo_samples = echo_samples_dblp; break;
    case AV_SAMPLE_FMT_FLTP: s->echo_samples = echo_samples_fltp; break;
    case AV_SAMPLE_FMT_S16P: s->echo_samples = echo_samples_s16p; break;
    case AV_SAMPLE_FMT_S32P: s->echo_samples = echo_samples_s32p; break;
    }


    if (s->delayptrs)
        av_freep(&s->delayptrs[0]);
    av_freep(&s->delayptrs);

    s->delay_index = av_calloc(outlink->ch_layout.nb_channels, sizeof(*s->delay_index));
    if (!s->delay_index)
        return AVERROR(ENOMEM);

    return av_samples_alloc_array_and_samples(&s->delayptrs, NULL,
                                              outlink->ch_layout.nb_channels,
                                              s->max_samples,
                                              outlink->format, 0);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AudioEchoContext *s = ctx->priv;
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
    ff_filter_execute(ctx, s->echo_samples, &td, NULL,
                      FFMIN(outlink->ch_layout.nb_channels, ff_filter_get_nb_threads(ctx)));

    s->next_pts = in->pts + av_rescale_q(in->nb_samples, (AVRational){1, inlink->sample_rate}, inlink->time_base);

    if (in != out)
        av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioEchoContext *s = ctx->priv;
    int nb_samples = FFMIN(s->fade_out, 2048);
    AVFrame *frame = ff_get_audio_buffer(outlink, nb_samples);
    ThreadData td;

    if (!frame)
        return AVERROR(ENOMEM);
    s->fade_out -= nb_samples;

    av_samples_set_silence(frame->extended_data, 0,
                           frame->nb_samples,
                           outlink->ch_layout.nb_channels,
                           frame->format);

    td.out = td.in = frame;
    ff_filter_execute(ctx, s->echo_samples, &td, NULL,
                      FFMIN(outlink->ch_layout.nb_channels, ff_filter_get_nb_threads(ctx)));

    frame->pts = s->next_pts;
    if (s->next_pts != AV_NOPTS_VALUE)
        s->next_pts += av_rescale_q(nb_samples, (AVRational){1, outlink->sample_rate}, outlink->time_base);

    return ff_filter_frame(outlink, frame);
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AudioEchoContext *s = ctx->priv;
    AVFrame *in;
    int ret, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = ff_inlink_consume_frame(inlink, &in);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return filter_frame(inlink, in);

    if (!s->eof && ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (status == AVERROR_EOF)
            s->eof = 1;
    }

    if (s->eof && s->fade_out <= 0) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->next_pts);
        return 0;
    }

    if (!s->eof)
        FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return request_frame(outlink);
}

static const AVFilterPad aecho_outputs[] = {
    {
        .name          = "default",
        .config_props  = config_output,
        .type          = AVMEDIA_TYPE_AUDIO,
    },
};

const AVFilter ff_af_aecho = {
    .name          = "aecho",
    .description   = NULL_IF_CONFIG_SMALL("Add echoing to the audio."),
    .priv_size     = sizeof(AudioEchoContext),
    .priv_class    = &aecho_class,
    .init          = init,
    .activate      = activate,
    .uninit        = uninit,
    .flags         = AVFILTER_FLAG_SLICE_THREADS,
    FILTER_INPUTS(ff_audio_default_filterpad),
    FILTER_OUTPUTS(aecho_outputs),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S32P,
                      AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_DBLP),
};
