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

#undef ftype
#undef MIN
#undef INVERT
#undef SAMPLE_FORMAT
#if DEPTH == 16
#define MIN INT16_MIN
#define INVERT(x) (-(x+(x==MIN)))
#define SAMPLE_FORMAT s16p
#define ftype int16_t
#elif DEPTH == 31
#define MIN INT32_MIN
#define INVERT(x) (-(x+(x==MIN)))
#define SAMPLE_FORMAT s32p
#define ftype int32_t
#elif DEPTH == 32
#define INVERT(x) (-(x))
#define SAMPLE_FORMAT fltp
#define ftype float
#elif DEPTH == 63
#define MIN INT64_MIN
#define INVERT(x) (-(x+(x==MIN)))
#define SAMPLE_FORMAT s64p
#define ftype int64_t
#else
#define INVERT(x) (-(x))
#define SAMPLE_FORMAT dblp
#define ftype double
#endif

#define fn3(a,b)   a##_##b
#define fn2(a,b)   fn3(a,b)
#define fn(a)      fn2(a, SAMPLE_FORMAT)

static int fn(filter_channels)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    ThreadData *td = arg;
    AVFrame *out = td->out;
    AVFrame *in = td->in;
    const int nb_channels = out->ch_layout.nb_channels;
    const int start = (nb_channels * jobnr) / nb_jobs;
    const int end = (nb_channels * (jobnr+1)) / nb_jobs;
    const int nb_samples = in->nb_samples;
    AudioInvertContext *s = ctx->priv;

    for (int ch = start; ch < end; ch++) {
        enum AVChannel channel = av_channel_layout_channel_from_index(&in->ch_layout, ch);
        const int bypass = av_channel_layout_index_from_channel(&s->ch_layout, channel) < 0;
        const ftype *src = (const ftype *)in->extended_data[ch];
        ftype *dst = (ftype *)out->extended_data[ch];

        if (bypass) {
            if (in != out)
                memcpy(dst, src, nb_samples * sizeof(*dst));
            continue;
        }

        for (int n = 0; n < nb_samples; n++)
            dst[n] = INVERT(src[n]);
    }

    return 0;
}
