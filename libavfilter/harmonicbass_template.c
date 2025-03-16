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

#undef FABS
#undef ftype
#undef SAMPLE_FORMAT
#if DEPTH == 32
#define FABS fabsf
#define ftype float
#define SAMPLE_FORMAT fltp
#else
#define FABS fabs
#define ftype double
#define SAMPLE_FORMAT dblp
#endif

#define F(x) ((ftype)(x))

#define fn3(a,b)   a##_##b
#define fn2(a,b)   fn3(a,b)
#define fn(a)      fn2(a, SAMPLE_FORMAT)

#define SQR(x) ((x) * (x))

static ftype fn(saturate)(const ftype sample, const ftype bias)
{
    ftype satOdd = sample / (FABS(sample) + F(1.0));
    ftype satEven = SQR(satOdd) * F(2.0);

    return satOdd + bias * (satEven - satOdd);
}

static void fn(hb_stereo)(AVFilterContext *ctx, AVFrame *out, AVFrame *in)
{
    AudioHarmonicBassContext *s = ctx->priv;
    const ftype *lsrc = (const ftype *)in->extended_data[0];
    const ftype *rsrc = (const ftype *)in->extended_data[1];
    ftype *ldst = (ftype *)out->extended_data[0];
    ftype *rdst = (ftype *)out->extended_data[1];
    ftype *lfe = (ftype *)out->extended_data[2];
    const int nb_samples = in->nb_samples;
    const ftype drive = s->hdrive;
    const ftype bias = s->hbias;
    const ftype hl = s->hlevel;
    const ftype sl = s->slevel;
    const ftype sa0 = s->sa[0];
    const ftype sa1 = s->sa[1];
    const ftype sa2 = s->sa[2];
    const ftype sm2 = s->sm[2];
    const ftype ha0 = s->ha[0];
    const ftype ha1 = s->ha[1];
    const ftype ha2 = s->ha[2];
    const ftype hm2 = s->hm[2];
    ftype hb0 = s->hcf[0];
    ftype hb1 = s->hcf[1];
    ftype sb0 = s->scf[0];
    ftype sb1 = s->scf[1];

    memcpy(ldst, lsrc, nb_samples * sizeof(*ldst));
    memcpy(rdst, rsrc, nb_samples * sizeof(*rdst));

    for (int n = 0; n < nb_samples; n++) {
        const ftype center = (lsrc[n] + rsrc[n]) * F(0.5);
        ftype v0 = center;
        ftype v3 = v0 - sb1;
        ftype v1 = sa0 * sb0 + sa1 * v3;
        ftype v2 = sb1 + sa1 * sb0 + sa2 * v3;
        ftype obass, bass, harmonics;

        sb0 = F(2.0) * v1 - sb0;
        sb1 = F(2.0) * v2 - sb1;

        bass = obass = sm2 * v2;
        bass = fn(saturate)(bass * drive, bias);

        v0 = bass;
        v3 = v0 - hb1;
        v1 = ha0 * hb0 + ha1 * v3;
        v2 = hb1 + ha1 * hb0 + ha2 * v3;

        hb0 = F(2.0) * v1 - hb0;
        hb1 = F(2.0) * v2 - hb1;

        harmonics = hm2 * v2;

        lfe[n] = harmonics * hl + obass * sl;
    }

    s->scf[0] = isnormal(sb0) ? sb0 : F(0.0);
    s->scf[1] = isnormal(sb1) ? sb1 : F(0.0);

    s->hcf[0] = isnormal(hb0) ? hb0 : F(0.0);
    s->hcf[1] = isnormal(hb1) ? hb1 : F(0.0);
}
