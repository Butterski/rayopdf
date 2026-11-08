// SPDX-License-Identifier: Apache-2.0
// Reference dumper for RayoPDF's JPX decoder check: decodes a JP2 file or a raw J2K codestream
// with libopenjp2 (single thread) and writes the decoded components as the library returns them
// (palette and channel definitions applied unless IGNORE is 1; no colour conversion).
//
// Usage: jpx_ref IN OUT [REDUCE [IGNORE]]
// OUT: "JPXREF1\0", then little-endian u32 values: decode microseconds, component count, colour
// space (OPJ_COLOR_SPACE), then per component w, h, x0, y0, dx, dy, prec, sgnd, alpha, then the
// samples of every component in order as int32. Exit status 0 when decoded, 1 otherwise.
#include <openjpeg.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void Quiet(const char* msg, void* data) {
    (void)msg;
    (void)data;
}

static void PutU32(FILE* f, uint32_t v) {
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    fwrite(b, 1, 4, f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: jpx_ref IN OUT [REDUCE [IGNORE]]\n");
        return 2;
    }
    const int reduce = argc > 3 ? atoi(argv[3]) : 0;
    const int ignore = argc > 4 ? atoi(argv[4]) : 0;
    FILE* in = fopen(argv[1], "rb");
    if (!in) return 1;
    unsigned char sig[12] = {0};
    size_t got = fread(sig, 1, sizeof sig, in);
    fclose(in);
    const int jp2 = got >= 12 && memcmp(sig + 4, "jP  ", 4) == 0;
    opj_codec_t* codec = opj_create_decompress(jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
    opj_set_info_handler(codec, Quiet, NULL);
    opj_set_warning_handler(codec, Quiet, NULL);
    opj_set_error_handler(codec, Quiet, NULL);
    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    params.cp_reduce = (OPJ_UINT32)reduce;
    if (ignore) params.flags |= OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG;
    opj_stream_t* stream = opj_stream_create_default_file_stream(argv[1], OPJ_TRUE);
    opj_image_t* image = NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = opj_setup_decoder(codec, &params) && opj_read_header(stream, codec, &image) &&
             opj_decode(codec, stream, image) && opj_end_decompress(codec, stream);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    if (!ok || !image) {
        if (image) opj_image_destroy(image);
        return 1;
    }
    FILE* out = fopen(argv[2], "wb");
    if (!out) return 1;
    fwrite("JPXREF1", 1, 8, out);
    PutU32(out, (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000));
    PutU32(out, image->numcomps);
    PutU32(out, (uint32_t)image->color_space);
    for (OPJ_UINT32 c = 0; c < image->numcomps; ++c) {
        const opj_image_comp_t* comp = &image->comps[c];
        PutU32(out, comp->w);
        PutU32(out, comp->h);
        PutU32(out, comp->x0);
        PutU32(out, comp->y0);
        PutU32(out, comp->dx);
        PutU32(out, comp->dy);
        PutU32(out, comp->prec);
        PutU32(out, comp->sgnd);
        PutU32(out, comp->alpha);
    }
    for (OPJ_UINT32 c = 0; c < image->numcomps; ++c) {
        const opj_image_comp_t* comp = &image->comps[c];
        const size_t n = (size_t)comp->w * comp->h;
        for (size_t i = 0; i < n; ++i) PutU32(out, (uint32_t)(comp->data ? comp->data[i] : 0));
    }
    fclose(out);
    opj_image_destroy(image);
    return 0;
}
