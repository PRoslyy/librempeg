/*
 * Copyright (c) 2017 Paul B Mahol
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

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"

static const char *const var_names[] = {
    "w",        ///< width of the input video
    "h",        ///< height of the input video
    "val",      ///< input value for the pixel
    "ymin",
    "umin",
    "vmin",
    "amin",
    "ymax",
    "umax",
    "vmax",
    "amax",
    NULL
};

enum var_name {
    VAR_W,
    VAR_H,
    VAR_VAL,
    VAR_YMIN,
    VAR_UMIN,
    VAR_VMIN,
    VAR_AMIN,
    VAR_YMAX,
    VAR_UMAX,
    VAR_VMAX,
    VAR_AMAX,
    VAR_VARS_NB
};

enum Curves {
    MAGMA,
    INFERNO,
    PLASMA,
    VIRIDIS,
    TURBO,
    CIVIDIS,
    SOLAR,
    SPECTRAL,
    COOL,
    HEAT,
    FIERY,
    BLUES,
    GREEN,
    HELIX,
    NB_CURVES,
};

enum Presets {
    PRESET_MAGMA,
    PRESET_INFERNO,
    PRESET_PLASMA,
    PRESET_VIRIDIS,
    PRESET_TURBO,
    PRESET_CIVIDIS,
    PRESET_RANGE1,
    PRESET_RANGE2,
    PRESET_SHADOWS,
    PRESET_HIGHLIGHTS,
    PRESET_SOLAR,
    PRESET_NOMINAL,
    PRESET_PREFERRED,
    PRESET_TOTAL,
    PRESET_SPECTRAL,
    PRESET_COOL,
    PRESET_HEAT,
    PRESET_FIERY,
    PRESET_BLUES,
    PRESET_GREEN,
    PRESET_HELIX,
    NB_PRESETS,
};

typedef double (*curve_fun)(double x);

typedef struct Curve {
    double coef[3][8];
    double offset[3];
    curve_fun fun[3];
    int yuv;
} Curve;

typedef struct Fill {
    float fill[4];
} Fill;

typedef struct Range {
    AVRational start, end;
} Range;

typedef struct Preset {
    int nb_segments;
    const Range *ranges;
    const Curve *curves;
    const Fill  *fills;
} Preset;

static const Range full_range   = {{0, 1}, {1, 1}};
static const Range nominal_range[] = {{{0, 1}, {4096, 65536}}, {{4096, 65536}, {60161, 65536}}, {{60161, 65536}, {1, 1}}};
static const Range preferred_range[] = {{{0, 1}, {1280, 65536}}, {{1280, 65536}, {62977, 65536}}, {{62977, 65536}, {1, 1}}};
static const Range total_range[] = {{{0, 1}, {256, 65536}}, {{256, 65536}, {65280, 65536}}, {{65280, 65536}, {1, 1}}};
static const Range spec1_range[] = {{{0, 1}, {16, 256}}, {{16, 256}, {236, 256}}, {{236, 256}, {256, 256}}};
static const Range spec2_range[] = {{{0, 1}, {16, 256}}, {{16, 256}, {22, 256}}, {{22, 256}, {226, 256}}, {{226, 256}, {236, 256}}, {{236, 256}, {256, 256}}};
static const Range shadows_range[] = {{{0, 1}, {32, 256}}, {{32, 256}, {256, 256}}};
static const Range highlights_range[] = {{{0,1}, {214,256}}, {{214, 256}, {224, 256}}, {{224, 256}, {256, 256}}};

static const Fill spec1_fills[] = {{{0.5f, 0.f, .5f, 1.f}}, {{-1.f, -1.f, -1.f, 1.f}}, {{1.f, 0.f, 0.f, 1.f}}};
static const Fill spec2_fills[] = {{{0.5f, 0.f, .5f, 1.f}}, {{0.f, 1.f, 1.f, 1.f}}, {{-1.f, -1.f, -1.f, 1.f}}, {{1.f, 1.f, 0.f, 1.f}}, {{1.f, 0.f, 0.f, 1.f}}};
static const Fill shadows_fills[] = {{{0.8f, 0.4f, .8f, 1.f}}, {{-1.f, -1.f, -1.f, 1.f}}};
static const Fill highlights_fills[] = {{{-1.f, -1.f, -1.f, 1.f}}, {{1.f, 0.3f, 0.6f, 1.f}}, {{1.f, 0.2f, .5f, 1.f}}};

static double limit(double x)
{
    return av_clipd(x, 0., 1.);
}

static double solarfun(double x)
{
    return 0.5 * sin(x) + 0.5;
}

static double coolfunu(double x)
{
    return 0.25 * sin(2.0 * x * M_PI - M_PI) + 0.5;
}

static double coolfunv(double x)
{
    return 0.25 * sin(2.0 * x * M_PI) + 0.5;
}

static double heatfunu(double x)
{
    return 0.25 * cos(2.0 * x * M_PI + M_PI) + 0.75;
}

static double heatfunv(double x)
{
    return 0.25 * sin(2.0 * x * M_PI) + 0.5;
}

static double fieryfunu(double x)
{
    return 0.75 - 0.25 * cos(2.0 * x * M_PI);
}

static double fieryfunv(double x)
{
    return 0.25 + 0.25 * cos(2.0 * x * M_PI);
}

static double helixfunu(double x)
{
    return 0.5 + 0.15 * sin(5.0 * x * M_PI + M_PI);
}

static double helixfunv(double x)
{
    return 0.5 + 0.15 * cos(6.0 * x * M_PI + M_PI_2);
}

static const Curve curves[] =
{
    [MAGMA] = {{
        {-7.5631093e-16,  7.4289183e-13, -2.8525484e-10,  5.4446085e-08, -5.5596238e-06,  3.0569325e-04, -2.3137421e-03,  1.2152095e-02 },
        { 1.3217636e-15, -1.2214648e-12,  4.4319712e-10, -8.0197993e-08,  7.6598370e-06, -3.6523704e-04,  8.4836670e-03, -2.5536888e-02 },
        {-1.1446568e-15,  1.0013446e-12, -3.5651575e-10,  6.6775016e-08, -6.7120346e-06,  2.7346619e-04,  4.7969657e-03,  1.1971441e-02 },
    }, .fun = { limit, limit, limit }, },
    [INFERNO] = {{
        {-3.9848859e-18,  9.4821649e-14, -6.7371977e-11,  1.8469937e-08, -2.5359307e-06,  1.7959053e-04,  3.9782564e-04,  2.8845935e-04 },
        { 6.8408539e-16, -6.5499979e-13,  2.4562526e-10, -4.5989298e-08,  4.5723324e-06, -2.2111913e-04,  5.2023164e-03, -1.1226064e-02 },
        {-2.9921470e-15,  2.5864165e-12, -8.7403799e-10,  1.4713388e-07, -1.2701505e-05,  4.5159935e-04,  3.1087989e-03,  1.9122831e-02 },
    }, .fun = { limit, limit, limit }, },
    [PLASMA] = {{
        { 3.6196089e-16, -3.3623041e-13,  1.2324010e-10, -2.2769060e-08,  2.2297792e-06, -1.2567829e-04,  9.9791629e-03,  5.7247918e-02 },
        { 5.0262888e-16, -5.3193896e-13,  2.2451715e-10, -4.7529623e-08,  5.1374873e-06, -2.3260136e-04,  3.1502825e-03,  1.5362491e-02 },
        {-1.7782261e-16,  2.2487839e-13, -1.0610236e-10,  2.4112644e-08, -2.6331623e-06,  8.9499751e-05,  2.1386328e-03,  5.3824268e-01 },
    }, .fun = { limit, limit, limit }, },
    [VIRIDIS] = {{
        { 9.4850045e-16, -8.6629383e-13,  3.0310944e-10, -5.1340396e-08,  4.6024275e-06, -2.2744239e-04,  4.5559993e-03,  2.5662350e-01 },
        { 9.6461041e-17, -6.9209477e-14,  1.7625397e-11, -2.0229773e-09,  1.4900110e-07, -1.9315187e-05,  5.8967339e-03,  3.9544827e-03 },
        { 5.1785449e-16, -3.6663004e-13,  1.0249990e-10, -1.5431998e-08,  1.5007941e-06, -1.2001502e-04,  7.6951526e-03,  3.2292815e-01 },
    }, .fun = { limit, limit, limit }, },
    [TURBO] = {{
        {-4.3683890e-15,  3.7020347e-12, -1.1712592e-09,  1.6401790e-07, -8.6842919e-06, -1.8542465e-06,  8.4485325e-03,  1.6267077e-01 },
        {-4.0011069e-16,  2.7861423e-13, -6.3388921e-11,  5.8872238e-09, -5.4466522e-07,  1.8037114e-05,  1.0599869e-02,  7.6914696e-02 },
        {-2.8242609e-15,  2.9234108e-12, -1.1726546e-09,  2.2552115e-07, -2.0059387e-05,  5.0595552e-04,  1.7714932e-02,  2.7271836e-01 },
    }, .fun = { limit, limit, limit }, },
    [CIVIDIS] = {{
        {-9.5484131e-16,  9.6988184e-13, -4.0058766e-10,  8.5743924e-08, -9.9644797e-06,  5.9197908e-04, -1.0361579e-02,  3.3164429e-02 },
        { 1.2731941e-17, -9.4238449e-15,  2.2808841e-12, -1.1548296e-10, -2.3888913e-08,  3.8986680e-06,  2.5879330e-03,  1.2769733e-01 },
        { 4.6004608e-16, -5.0686849e-13,  2.2753449e-10, -5.3074099e-08,  6.7196096e-06, -4.4120020e-04,  1.3435551e-02,  2.8293355e-01 },
    }, .fun = { limit, limit, limit }, },
    [SOLAR] = {{
        { 0, 0, 0, 0, 0.000001983938313, -0.0007618323, 0.2, -M_PI_2 },
        { 0, 0, 0, 0, 0.000001983938313, -0.0007618323, 0.2, -M_PI_2 },
        { 0, 0, 0, 0, 0.000001983938313, -0.0007618323, 0.2, -M_PI_2 },
    },
    .offset = { 0., -9., 9. },
    .fun = { solarfun, solarfun, solarfun }, },
    [SPECTRAL] = {{
        { -1.6820e-15,   1.4982e-12,  -5.0442e-10,   8.0490e-08,  -6.1903e-06,   1.5821e-04, 6.4359e-03,   6.2887e-01 },
        {  1.2526e-15,  -1.2203e-12,   4.7013e-10,  -8.9360e-08,   8.3839e-06,  -3.6642e-04, 1.4784e-02,  -9.8075e-03 },
        {  1.4755e-15,  -1.6765e-12,   7.3188e-10,  -1.5522e-07,   1.6406e-05,  -7.7883e-04, 1.4502e-02,   2.1597e-01 },
    }, .fun = { limit, limit, limit }, },
    [COOL] = {{
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
    },
    .offset = { 0., 0., 0 },
    .yuv = 1,
    .fun = { coolfunu, limit, coolfunv }, },
    [HEAT] = {{
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
    },
    .offset = { 0., 0., 0 },
    .yuv = 1,
    .fun = { heatfunu, limit, heatfunv }, },
    [FIERY] = {{
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
    },
    .offset = { 0., 0., 0 },
    .yuv = 1,
    .fun = { fieryfunu, limit, fieryfunv }, },
    [BLUES] = {{
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
    },
    .offset = { 0., 0., 0 },
    .yuv = 1,
    .fun = { fieryfunv, limit, fieryfunu }, },
    [GREEN] = {{
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
    },
    .offset = { 0., 0., 0 },
    .yuv = 1,
    .fun = { fieryfunv, limit, fieryfunv }, },
    [HELIX] = {{
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
        { 0, 0, 0, 0, 0, 0, 1./256, 0 },
    },
    .offset = { 0., 0., 0 },
    .yuv = 1,
    .fun = { helixfunu, limit, helixfunv }, },
};

static const Preset presets[] =
{
    [PRESET_MAGMA]   = { 1, &full_range, &curves[MAGMA],   NULL },
    [PRESET_INFERNO] = { 1, &full_range, &curves[INFERNO], NULL },
    [PRESET_PLASMA]  = { 1, &full_range, &curves[PLASMA],  NULL },
    [PRESET_VIRIDIS] = { 1, &full_range, &curves[VIRIDIS], NULL },
    [PRESET_TURBO]   = { 1, &full_range, &curves[TURBO],   NULL },
    [PRESET_CIVIDIS] = { 1, &full_range, &curves[CIVIDIS], NULL },
    [PRESET_RANGE1]  = { 3, spec1_range, NULL,             spec1_fills },
    [PRESET_NOMINAL] = { 3, nominal_range, NULL,           spec1_fills },
    [PRESET_PREFERRED]={ 3, preferred_range, NULL,         spec1_fills },
    [PRESET_TOTAL]   = { 3, total_range, NULL,             spec1_fills },
    [PRESET_RANGE2]  = { 5, spec2_range, NULL,             spec2_fills },
    [PRESET_SHADOWS] = { 2, shadows_range, NULL,           shadows_fills },
    [PRESET_HIGHLIGHTS] = { 3, highlights_range, NULL,     highlights_fills },
    [PRESET_SOLAR]   = { 1, &full_range, &curves[SOLAR],   NULL },
    [PRESET_SPECTRAL]= { 1, &full_range, &curves[SPECTRAL],NULL },
    [PRESET_COOL]    = { 1, &full_range, &curves[COOL],    NULL },
    [PRESET_HEAT]    = { 1, &full_range, &curves[HEAT],    NULL },
    [PRESET_FIERY]   = { 1, &full_range, &curves[FIERY],   NULL },
    [PRESET_BLUES]   = { 1, &full_range, &curves[BLUES],   NULL },
    [PRESET_GREEN]   = { 1, &full_range, &curves[GREEN],   NULL },
    [PRESET_HELIX]   = { 1, &full_range, &curves[HELIX],   NULL },
};

typedef struct PseudoColorContext {
    const AVClass *class;
    int preset;
    float opacity;
    int max;
    int index;
    int nb_planes;
    int color;
    int linesize[4];
    int width[4], height[4];
    double var_values[VAR_VARS_NB];
    char   *comp_expr_str[4];
    AVExpr *comp_expr[4];
    float lut[4][256*256];

    void (*filter[4])(int max, int width, int height,
                      const uint8_t *index, const uint8_t *src,
                      uint8_t *dst,
                      ptrdiff_t ilinesize,
                      ptrdiff_t slinesize,
                      ptrdiff_t dlinesize,
                      float *lut,
                      float opacity);
} PseudoColorContext;

#define OFFSET(x) offsetof(PseudoColorContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption pseudocolor_options[] = {
    { "c0", "set component #0 expression", OFFSET(comp_expr_str[0]), AV_OPT_TYPE_STRING, {.str="val"},   .flags = FLAGS },
    { "c1", "set component #1 expression", OFFSET(comp_expr_str[1]), AV_OPT_TYPE_STRING, {.str="val"},   .flags = FLAGS },
    { "c2", "set component #2 expression", OFFSET(comp_expr_str[2]), AV_OPT_TYPE_STRING, {.str="val"},   .flags = FLAGS },
    { "c3", "set component #3 expression", OFFSET(comp_expr_str[3]), AV_OPT_TYPE_STRING, {.str="val"},   .flags = FLAGS },
    { "index", "set component as base",    OFFSET(index),            AV_OPT_TYPE_INT,    {.i64=0}, 0, 3, .flags = FLAGS },
    { "i",  "set component as base",       OFFSET(index),            AV_OPT_TYPE_INT,    {.i64=0}, 0, 3, .flags = FLAGS },
    { "preset", "set preset",              OFFSET(preset),           AV_OPT_TYPE_INT,    {.i64=-1},-1, NB_PRESETS-1, .flags = FLAGS, .unit = "preset" },
    { "p",  "set preset",                  OFFSET(preset),           AV_OPT_TYPE_INT,    {.i64=-1},-1, NB_PRESETS-1, .flags = FLAGS, .unit = "preset" },
    { "none",       NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=-1},             .flags = FLAGS, .unit = "preset" },
    { "magma",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_MAGMA},   .flags = FLAGS, .unit = "preset" },
    { "inferno",    NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_INFERNO}, .flags = FLAGS, .unit = "preset" },
    { "plasma",     NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_PLASMA},  .flags = FLAGS, .unit = "preset" },
    { "viridis",    NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_VIRIDIS}, .flags = FLAGS, .unit = "preset" },
    { "turbo",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_TURBO},   .flags = FLAGS, .unit = "preset" },
    { "cividis",    NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_CIVIDIS}, .flags = FLAGS, .unit = "preset" },
    { "range1",     NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_RANGE1},  .flags = FLAGS, .unit = "preset" },
    { "range2",     NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_RANGE2},  .flags = FLAGS, .unit = "preset" },
    { "shadows",    NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_SHADOWS}, .flags = FLAGS, .unit = "preset" },
    { "highlights", NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_HIGHLIGHTS},.flags=FLAGS, .unit = "preset" },
    { "solar",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_SOLAR},   .flags=FLAGS,   .unit = "preset" },
    { "nominal",    NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_NOMINAL}, .flags=FLAGS,   .unit = "preset" },
    { "preferred",  NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_PREFERRED},.flags=FLAGS,  .unit = "preset" },
    { "total",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_TOTAL},   .flags=FLAGS,   .unit = "preset" },
    { "spectral",   NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_SPECTRAL},.flags = FLAGS, .unit = "preset" },
    { "cool",       NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_COOL},    .flags = FLAGS, .unit = "preset" },
    { "heat",       NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_HEAT},    .flags = FLAGS, .unit = "preset" },
    { "fiery",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_FIERY},   .flags = FLAGS, .unit = "preset" },
    { "blues",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_BLUES},   .flags = FLAGS, .unit = "preset" },
    { "green",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_GREEN},   .flags = FLAGS, .unit = "preset" },
    { "helix",      NULL,                  0,                        AV_OPT_TYPE_CONST,  {.i64=PRESET_HELIX},   .flags = FLAGS, .unit = "preset" },
    { "opacity", "set pseudocolor opacity",OFFSET(opacity),          AV_OPT_TYPE_FLOAT,  {.dbl=1}, 0, 1, .flags = FLAGS },
    { NULL }
};

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY9, AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12, AV_PIX_FMT_GRAY14, AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVA422P,
    AV_PIX_FMT_YUV444P, AV_PIX_FMT_GBRP,
    AV_PIX_FMT_YUVA444P, AV_PIX_FMT_GBRAP,
    AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUVA422P9,
    AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUVA420P9,
    AV_PIX_FMT_YUV444P9, AV_PIX_FMT_YUVA444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUVA420P10,
    AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUVA422P10,
    AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUVA444P10,
    AV_PIX_FMT_YUV420P12,
    AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUVA422P12,
    AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUVA444P12,
    AV_PIX_FMT_YUV420P14,
    AV_PIX_FMT_YUV422P14,
    AV_PIX_FMT_YUV444P14,
    AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUVA420P16,
    AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUVA422P16,
    AV_PIX_FMT_YUV444P16, AV_PIX_FMT_YUVA444P16,
    AV_PIX_FMT_GBRP9,
    AV_PIX_FMT_GBRP10, AV_PIX_FMT_GBRAP10,
    AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRAP12,
    AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRAP14,
    AV_PIX_FMT_GBRP16, AV_PIX_FMT_GBRAP16,
    AV_PIX_FMT_NONE
};

static inline float lerpf(float v0, float v1, float f)
{
    return v0 + (v1 - v0) * f;
}

#define PCLIP(v, max, dst, src, x) \
    if (v >= 0 && v <= max) {      \
        dst[x] = lerpf(src[x], v, opacity);\
    } else {                       \
        dst[x] = src[x];           \
    }

static void pseudocolor_filter(int max, int width, int height,
                               const uint8_t *index,
                               const uint8_t *src,
                               uint8_t *dst,
                               ptrdiff_t ilinesize,
                               ptrdiff_t slinesize,
                               ptrdiff_t dlinesize,
                               float *lut,
                               float opacity)
{
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[x]];

            PCLIP(v, max, dst, src, x);
        }
        index += ilinesize;
        src += slinesize;
        dst += dlinesize;
    }
}

static void pseudocolor_filter_11(int max, int width, int height,
                                  const uint8_t *index,
                                  const uint8_t *src,
                                  uint8_t *dst,
                                  ptrdiff_t ilinesize,
                                  ptrdiff_t slinesize,
                                  ptrdiff_t dlinesize,
                                  float *lut,
                                  float opacity)
{
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[(y << 1) * ilinesize + (x << 1)]];

            PCLIP(v, max, dst, src, x);
        }
        src += slinesize;
        dst += dlinesize;
    }
}

static void pseudocolor_filter_11d(int max, int width, int height,
                                   const uint8_t *index,
                                   const uint8_t *src,
                                   uint8_t *dst,
                                   ptrdiff_t ilinesize,
                                   ptrdiff_t slinesize,
                                   ptrdiff_t dlinesize,
                                   float *lut,
                                   float opacity)
{
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[(y >> 1) * ilinesize + (x >> 1)]];

            PCLIP(v, max, dst, src, x);
        }
        src += slinesize;
        dst += dlinesize;
    }
}

static void pseudocolor_filter_10(int max, int width, int height,
                                  const uint8_t *index,
                                  const uint8_t *src,
                                  uint8_t *dst,
                                  ptrdiff_t ilinesize,
                                  ptrdiff_t slinesize,
                                  ptrdiff_t dlinesize,
                                  float *lut,
                                  float opacity)
{
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[x << 1]];

            PCLIP(v, max, dst, src, x);
        }
        index += ilinesize;
        src += slinesize;
        dst += dlinesize;
    }
}

static void pseudocolor_filter_10d(int max, int width, int height,
                                   const uint8_t *index,
                                   const uint8_t *src,
                                   uint8_t *dst,
                                   ptrdiff_t ilinesize,
                                   ptrdiff_t slinesize,
                                   ptrdiff_t dlinesize,
                                   float *lut,
                                   float opacity)
{
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[x >> 1]];

            PCLIP(v, max, dst, src, x);
        }
        index += ilinesize;
        src += slinesize;
        dst += dlinesize;
    }
}

static void pseudocolor_filter_16(int max, int width, int height,
                                  const uint8_t *iindex,
                                  const uint8_t *ssrc,
                                  uint8_t *ddst,
                                  ptrdiff_t ilinesize,
                                  ptrdiff_t slinesize,
                                  ptrdiff_t dlinesize,
                                  float *lut,
                                  float opacity)
{
    const uint16_t *index = (const uint16_t *)iindex;
    const uint16_t *src = (const uint16_t *)ssrc;
    uint16_t *dst = (uint16_t *)ddst;
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[x]];

            PCLIP(v, max, dst, src, x);
        }
        index += ilinesize / 2;
        src += slinesize / 2;
        dst += dlinesize / 2;
    }
}

static void pseudocolor_filter_16_10(int max, int width, int height,
                                     const uint8_t *iindex,
                                     const uint8_t *ssrc,
                                     uint8_t *ddst,
                                     ptrdiff_t ilinesize,
                                     ptrdiff_t slinesize,
                                     ptrdiff_t dlinesize,
                                     float *lut,
                                     float opacity)
{
    const uint16_t *index = (const uint16_t *)iindex;
    const uint16_t *src = (const uint16_t *)ssrc;
    uint16_t *dst = (uint16_t *)ddst;
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[x << 1]];

            PCLIP(v, max, dst, src, x);
        }
        index += ilinesize / 2;
        src += slinesize / 2;
        dst += dlinesize / 2;
    }
}

static void pseudocolor_filter_16_10d(int max, int width, int height,
                                      const uint8_t *iindex,
                                      const uint8_t *ssrc,
                                      uint8_t *ddst,
                                      ptrdiff_t ilinesize,
                                      ptrdiff_t slinesize,
                                      ptrdiff_t dlinesize,
                                      float *lut,
                                      float opacity)
{
    const uint16_t *index = (const uint16_t *)iindex;
    const uint16_t *src = (const uint16_t *)ssrc;
    uint16_t *dst = (uint16_t *)ddst;
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[x >> 1]];

            PCLIP(v, max, dst, src, x);
        }
        index += ilinesize / 2;
        src += slinesize / 2;
        dst += dlinesize / 2;
    }
}

static void pseudocolor_filter_16_11(int max, int width, int height,
                                     const uint8_t *iindex,
                                     const uint8_t *ssrc,
                                     uint8_t *ddst,
                                     ptrdiff_t ilinesize,
                                     ptrdiff_t slinesize,
                                     ptrdiff_t dlinesize,
                                     float *lut,
                                     float opacity)
{
    const uint16_t *index = (const uint16_t *)iindex;
    const uint16_t *src = (const uint16_t *)ssrc;
    uint16_t *dst = (uint16_t *)ddst;
    int x, y;

    ilinesize /= 2;
    dlinesize /= 2;
    slinesize /= 2;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[(y << 1) * ilinesize + (x << 1)]];

            PCLIP(v, max, dst, src, x);
        }
        src += slinesize;
        dst += dlinesize;
    }
}

static void pseudocolor_filter_16_11d(int max, int width, int height,
                                      const uint8_t *iindex,
                                      const uint8_t *ssrc,
                                      uint8_t *ddst,
                                      ptrdiff_t ilinesize,
                                      ptrdiff_t slinesize,
                                      ptrdiff_t dlinesize,
                                      float *lut,
                                      float opacity)
{
    const uint16_t *index = (const uint16_t *)iindex;
    const uint16_t *src = (const uint16_t *)ssrc;
    uint16_t *dst = (uint16_t *)ddst;
    int x, y;

    ilinesize /= 2;
    dlinesize /= 2;
    slinesize /= 2;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int v = lut[index[(y >> 1) * ilinesize + (x >> 1)]];

            PCLIP(v, max, dst, src, x);
        }
        src += slinesize;
        dst += dlinesize;
    }
}

#define RGB_TO_Y_BT709(r, g, b) \
((0.21260*219.0/255.0) * (r) + (0.71520*219.0/255.0) * (g) + \
 (0.07220*219.0/255.0) * (b))

#define RGB_TO_U_BT709(r1, g1, b1, max) \
(-(0.11457*224.0/255.0) * r1 - (0.38543*224.0/255.0) * g1 + \
    (0.50000*224.0/255.0) * b1 + max * 0.5)

#define RGB_TO_V_BT709(r1, g1, b1, max) \
((0.50000*224.0/255.0) * r1 - (0.45415*224.0/255.0) * g1 - \
   (0.04585*224.0/255.0) * b1 + max * 0.5)

#define Wr 0.2126
#define Wb 0.0722
#define Wg (1 - Wr - Wb)
#define Umax 0.436
#define Vmax 0.615

#define YUV_BT709_TO_R(y, u, v, max) \
    ((y + v * (1 - Wr) / Vmax) * max)
#define YUV_BT709_TO_G(y, u, v, max) \
    ((y - (u * Wb * (1 - Wb) / (Umax * Wg)) - (v * Wr * (1 - Wr) / (Vmax * Wg))) * max)
#define YUV_BT709_TO_B(y, u, v, max) \
    ((y + u * (1 - Wb) / Umax) * max)

static double poly_eval(const double *const poly, double x, curve_fun fun)
{
    double res = 0.;

    for (int i = 0; i < 8; i++) {
        res += pow(x, i) * poly[7-i];
    }

    return fun(res);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    PseudoColorContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    int depth, ret, hsub, vsub, color, rgb;

    rgb = desc->flags & AV_PIX_FMT_FLAG_RGB;
    depth = desc->comp[0].depth;
    s->max = (1 << depth) - 1;
    s->nb_planes = av_pix_fmt_count_planes(inlink->format);

    if (s->index >= s->nb_planes) {
        av_log(ctx, AV_LOG_ERROR, "index out of allowed range\n");
        return AVERROR(EINVAL);
    }

    if ((ret = av_image_fill_linesizes(s->linesize, inlink->format, inlink->w)) < 0)
        return ret;

    hsub = desc->log2_chroma_w;
    vsub = desc->log2_chroma_h;
    s->height[1] = s->height[2] = AV_CEIL_RSHIFT(inlink->h, vsub);
    s->height[0] = s->height[3] = inlink->h;
    s->width[1]  = s->width[2]  = AV_CEIL_RSHIFT(inlink->w, hsub);
    s->width[0]  = s->width[3]  = inlink->w;

    s->var_values[VAR_W] = inlink->w;
    s->var_values[VAR_H] = inlink->h;

    s->var_values[VAR_YMIN] = 16 * (1 << (depth - 8));
    s->var_values[VAR_UMIN] = 16 * (1 << (depth - 8));
    s->var_values[VAR_VMIN] = 16 * (1 << (depth - 8));
    s->var_values[VAR_AMIN] = 0;
    s->var_values[VAR_YMAX] = 235 * (1 << (depth - 8));
    s->var_values[VAR_UMAX] = 240 * (1 << (depth - 8));
    s->var_values[VAR_VMAX] = 240 * (1 << (depth - 8));
    s->var_values[VAR_AMAX] = s->max;

    for (color = 0; color < s->nb_planes && s->preset < 0; color++) {
        double res;
        int val;

        /* create the parsed expression */
        av_expr_free(s->comp_expr[color]);
        s->comp_expr[color] = NULL;
        ret = av_expr_parse(&s->comp_expr[color], s->comp_expr_str[color],
                            var_names, NULL, NULL, NULL, NULL, 0, ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Error when parsing the expression '%s' for the component %d and color %d.\n",
                   s->comp_expr_str[color], color, color);
            return AVERROR(EINVAL);
        }

        /* compute the lut */
        for (val = 0; val < FF_ARRAY_ELEMS(s->lut[color]); val++) {
            s->var_values[VAR_VAL] = val;

            res = av_expr_eval(s->comp_expr[color], s->var_values, s);
            if (isnan(res)) {
                av_log(ctx, AV_LOG_ERROR,
                       "Error when evaluating the expression '%s' for the value %d for the component %d.\n",
                       s->comp_expr_str[color], val, color);
                return AVERROR(EINVAL);
            }
            s->lut[color][val] = res;
        }
    }

    if (s->preset >= 0) {
        int nb_segments = presets[s->preset].nb_segments;

        for (int seg = 0; seg < nb_segments; seg++) {
            AVRational rstart = presets[s->preset].ranges[seg].start;
            AVRational rend   = presets[s->preset].ranges[seg].end;
            int start = av_rescale_rnd(s->max + 1, rstart.num, rstart.den, AV_ROUND_UP);
            int end   = av_rescale_rnd(s->max + 1, rend.num, rend.den, AV_ROUND_UP);

            for (int i = start; i < end; i++) {
                if (!presets[s->preset].curves) {
                    const Fill fill = presets[s->preset].fills[seg];
                    double r, g, b, a;

                    g = fill.fill[1];
                    b = fill.fill[2];
                    r = fill.fill[0];
                    a = fill.fill[3];

                    if (g >= 0.f && b >= 0.f && r >= 0.f) {
                        g *= s->max;
                        b *= s->max;
                        r *= s->max;

                        if (!rgb) {
                            double y = RGB_TO_Y_BT709(r, g, b);
                            double u = RGB_TO_U_BT709(r, g, b, s->max);
                            double v = RGB_TO_V_BT709(r, g, b, s->max);

                            r = v;
                            g = y;
                            b = u;
                        }
                    }

                    s->lut[0][i] = g;
                    s->lut[1][i] = b;
                    s->lut[2][i] = r;
                    s->lut[3][i] = a * s->max;
                } else {
                    const Curve curve = presets[s->preset].curves[seg];
                    const double lf = i / (double)s->max * 256.;
                    double r, g, b;

                    g = poly_eval(curve.coef[1], lf + curve.offset[1], curve.fun[1]);
                    b = poly_eval(curve.coef[2], lf + curve.offset[2], curve.fun[2]);
                    r = poly_eval(curve.coef[0], lf + curve.offset[0], curve.fun[0]);

                    if (!curve.yuv || !rgb) {
                        g *= s->max;
                        b *= s->max;
                        r *= s->max;
                    }

                    if (!rgb && !curve.yuv) {
                        double y = RGB_TO_Y_BT709(r, g, b);
                        double u = RGB_TO_U_BT709(r, g, b, s->max);
                        double v = RGB_TO_V_BT709(r, g, b, s->max);

                        r = v;
                        g = y;
                        b = u;
                    } else if (rgb && curve.yuv) {
                        double y = g;
                        double u = b - 0.5;
                        double v = r - 0.5;

                        r = av_clipd(YUV_BT709_TO_R(y, u, v, s->max), 0, s->max);
                        g = av_clipd(YUV_BT709_TO_G(y, u, v, s->max), 0, s->max);
                        b = av_clipd(YUV_BT709_TO_B(y, u, v, s->max), 0, s->max);
                    }

                    s->lut[0][i] = g;
                    s->lut[1][i] = b;
                    s->lut[2][i] = r;
                    s->lut[3][i] = 1.f * s->max;
                }
            }
        }
    }

    switch (inlink->format) {
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVA444P:
    case AV_PIX_FMT_GBRP:
    case AV_PIX_FMT_GBRAP:
    case AV_PIX_FMT_GRAY8:
        s->filter[0] = s->filter[1] = s->filter[2] = s->filter[3] = pseudocolor_filter;
        break;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVA420P:
        switch (s->index) {
        case 0:
        case 3:
            s->filter[0] = s->filter[3] = pseudocolor_filter;
            s->filter[1] = s->filter[2] = pseudocolor_filter_11;
            break;
        case 1:
        case 2:
            s->filter[0] = s->filter[3] = pseudocolor_filter_11d;
            s->filter[1] = s->filter[2] = pseudocolor_filter;
            break;
        }
        break;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVA422P:
        switch (s->index) {
        case 0:
        case 3:
            s->filter[0] = s->filter[3] = pseudocolor_filter;
            s->filter[1] = s->filter[2] = pseudocolor_filter_10;
            break;
        case 1:
        case 2:
            s->filter[0] = s->filter[3] = pseudocolor_filter_10d;
            s->filter[1] = s->filter[2] = pseudocolor_filter;
            break;
        }
        break;
    case AV_PIX_FMT_YUV444P9:
    case AV_PIX_FMT_YUVA444P9:
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_YUVA444P10:
    case AV_PIX_FMT_YUV444P12:
    case AV_PIX_FMT_YUVA444P12:
    case AV_PIX_FMT_YUV444P14:
    case AV_PIX_FMT_YUV444P16:
    case AV_PIX_FMT_YUVA444P16:
    case AV_PIX_FMT_GBRP9:
    case AV_PIX_FMT_GBRP10:
    case AV_PIX_FMT_GBRP12:
    case AV_PIX_FMT_GBRP14:
    case AV_PIX_FMT_GBRP16:
    case AV_PIX_FMT_GBRAP10:
    case AV_PIX_FMT_GBRAP12:
    case AV_PIX_FMT_GBRAP14:
    case AV_PIX_FMT_GBRAP16:
    case AV_PIX_FMT_GRAY9:
    case AV_PIX_FMT_GRAY10:
    case AV_PIX_FMT_GRAY12:
    case AV_PIX_FMT_GRAY14:
    case AV_PIX_FMT_GRAY16:
        s->filter[0] = s->filter[1] = s->filter[2] = s->filter[3] = pseudocolor_filter_16;
        break;
    case AV_PIX_FMT_YUV422P9:
    case AV_PIX_FMT_YUVA422P9:
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_YUVA422P10:
    case AV_PIX_FMT_YUV422P12:
    case AV_PIX_FMT_YUVA422P12:
    case AV_PIX_FMT_YUV422P14:
    case AV_PIX_FMT_YUV422P16:
    case AV_PIX_FMT_YUVA422P16:
        switch (s->index) {
        case 0:
        case 3:
            s->filter[0] = s->filter[3] = pseudocolor_filter_16;
            s->filter[1] = s->filter[2] = pseudocolor_filter_16_10;
            break;
        case 1:
        case 2:
            s->filter[0] = s->filter[3] = pseudocolor_filter_16_10d;
            s->filter[1] = s->filter[2] = pseudocolor_filter_16;
            break;
        }
        break;
    case AV_PIX_FMT_YUV420P9:
    case AV_PIX_FMT_YUVA420P9:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUVA420P10:
    case AV_PIX_FMT_YUV420P12:
    case AV_PIX_FMT_YUV420P14:
    case AV_PIX_FMT_YUV420P16:
    case AV_PIX_FMT_YUVA420P16:
        switch (s->index) {
        case 0:
        case 3:
            s->filter[0] = s->filter[3] = pseudocolor_filter_16;
            s->filter[1] = s->filter[2] = pseudocolor_filter_16_11;
            break;
        case 1:
        case 2:
            s->filter[0] = s->filter[3] = pseudocolor_filter_16_11d;
            s->filter[1] = s->filter[2] = pseudocolor_filter_16;
            break;
        }
        break;
    }

    return 0;
}

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    PseudoColorContext *s = ctx->priv;
    ThreadData *td = arg;
    AVFrame *in = td->in;
    AVFrame *out = td->out;

    for (int plane = 0; plane < s->nb_planes; plane++) {
        const int slice_start = (s->height[plane] * jobnr) / nb_jobs;
        const int slice_end = (s->height[plane] * (jobnr+1)) / nb_jobs;
        const int islice_start = (s->height[s->index] * jobnr) / nb_jobs;
        ptrdiff_t ilinesize = in->linesize[s->index];
        ptrdiff_t slinesize = in->linesize[plane];
        ptrdiff_t dlinesize = out->linesize[plane];
        const uint8_t *index = in->data[s->index] + islice_start * ilinesize;
        const uint8_t *src = in->data[plane] + slice_start * slinesize;
        uint8_t *dst = out->data[plane] + slice_start * dlinesize;

        s->filter[plane](s->max, s->width[plane], slice_end - slice_start,
                         index, src, dst, ilinesize, slinesize,
                         dlinesize, s->lut[plane], s->opacity);
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    PseudoColorContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ThreadData td;
    AVFrame *out;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    td.out = out, td.in = in;
    ff_filter_execute(ctx, filter_slice, &td, NULL,
                      FFMIN(s->height[1], ff_filter_get_nb_threads(ctx)));

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *arg)
{
    int ret = ff_filter_process_command(ctx, cmd, arg);

    if (ret < 0)
        return ret;

    return config_input(ctx->inputs[0]);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

static av_cold void uninit(AVFilterContext *ctx)
{
    PseudoColorContext *s = ctx->priv;
    int i;

    for (i = 0; i < 4; i++) {
        av_expr_free(s->comp_expr[i]);
        s->comp_expr[i] = NULL;
    }
}

AVFILTER_DEFINE_CLASS(pseudocolor);

const FFFilter ff_vf_pseudocolor = {
    .p.name        = "pseudocolor",
    .p.description = NULL_IF_CONFIG_SMALL("Make pseudocolored video frames."),
    .p.priv_class  = &pseudocolor_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
    .priv_size     = sizeof(PseudoColorContext),
    .uninit        = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .process_command = process_command,
};
