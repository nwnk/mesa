/*
 * Copyright (C) 2008 VMware, Inc.
 * Copyright (C) 2014 Broadcom
 * Copyright (C) 2018-2019 Alyssa Rosenzweig
 * Copyright (C) 2019-2020 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include "util/macros.h"
#include "util/u_math.h"
#include "pan_texture.h"

/* Generates a texture descriptor. Ideally, descriptors are immutable after the
 * texture is created, so we can keep these hanging around in GPU memory in a
 * dedicated BO and not have to worry. In practice there are some minor gotchas
 * with this (the driver sometimes will change the format of a texture on the
 * fly for compression) but it's fast enough to just regenerate the descriptor
 * in those cases, rather than monkeypatching at drawtime. A texture descriptor
 * consists of a 32-byte header followed by pointers. 
 */

/* List of supported modifiers, in descending order of preference. AFBC is
 * faster than u-interleaved tiling which is faster than linear. Within AFBC,
 * enabling the YUV-like transform is typically a win where possible. */

uint64_t pan_best_modifiers[PAN_MODIFIER_COUNT] = {
        DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                AFBC_FORMAT_MOD_SPARSE |
                AFBC_FORMAT_MOD_YTR),

        DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                AFBC_FORMAT_MOD_SPARSE),

        DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED,
        DRM_FORMAT_MOD_LINEAR
};

/* Map modifiers to mali_texture_layout for packing in a texture descriptor */

static enum mali_texture_layout
panfrost_modifier_to_layout(uint64_t modifier)
{
        if (drm_is_afbc(modifier))
                return MALI_TEXTURE_LAYOUT_AFBC;
        else if (modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED)
                return MALI_TEXTURE_LAYOUT_TILED;
        else if (modifier == DRM_FORMAT_MOD_LINEAR)
                return MALI_TEXTURE_LAYOUT_LINEAR;
        else
                unreachable("Invalid modifer");
}

/* Check if we need to set a custom stride by computing the "expected"
 * stride and comparing it to what the user actually wants. Only applies
 * to linear textures, since tiled/compressed textures have strict
 * alignment requirements for their strides as it is */

static bool
panfrost_needs_explicit_stride(
                struct panfrost_slice *slices,
                uint16_t width,
                unsigned first_level, unsigned last_level,
                unsigned bytes_per_pixel)
{
        for (unsigned l = first_level; l <= last_level; ++l) {
                unsigned actual = slices[l].stride;
                unsigned expected = u_minify(width, l) * bytes_per_pixel;

                if (actual != expected)
                        return true;
        }

        return false;
}

/* A Scalable Texture Compression (ASTC) corresponds to just a few texture type
 * in the hardware, but in fact can be parametrized to have various widths and
 * heights for the so-called "stretch factor". It turns out these parameters
 * are stuffed in the bottom bits of the payload pointers. This functions
 * computes these magic stuffing constants based on the ASTC format in use. The
 * constant in a given dimension is 3-bits, and two are stored side-by-side for
 * each active dimension.
 */

static unsigned
panfrost_astc_stretch(unsigned dim)
{
        assert(dim >= 4 && dim <= 12);
        return MIN2(dim, 11) - 4;
}

/* Texture addresses are tagged with information about compressed formats.
 * AFBC uses a bit for whether the colorspace transform is enabled (RGB and
 * RGBA only).
 * For ASTC, this is a "stretch factor" encoding the block size. */

static unsigned
panfrost_compression_tag(
                const struct util_format_description *desc,
                enum mali_format format, uint64_t modifier)
{
        if (drm_is_afbc(modifier))
                return (modifier & AFBC_FORMAT_MOD_YTR) ? 1 : 0;
        else if (format == MALI_ASTC_2D_LDR || format == MALI_ASTC_2D_HDR)
                return (panfrost_astc_stretch(desc->block.height) << 3) |
                        panfrost_astc_stretch(desc->block.width);
        else
                return 0;
}


/* Cubemaps have 6 faces as "layers" in between each actual layer. We
 * need to fix this up. TODO: logic wrong in the asserted out cases ...
 * can they happen, perhaps from cubemap arrays? */

static void
panfrost_adjust_cube_dimensions(
                unsigned *first_face, unsigned *last_face,
                unsigned *first_layer, unsigned *last_layer)
{
        *first_face = *first_layer % 6;
        *last_face = *last_layer % 6;
        *first_layer /= 6;
        *last_layer /= 6;

        assert((*first_layer == *last_layer) || (*first_face == 0 && *last_face == 5));
}

/* Following the texture descriptor is a number of pointers. How many? */

static unsigned
panfrost_texture_num_elements(
                unsigned first_level, unsigned last_level,
                unsigned first_layer, unsigned last_layer,
                unsigned nr_samples,
                bool is_cube, bool manual_stride)
{
        unsigned first_face  = 0, last_face = 0;

        if (is_cube) {
                panfrost_adjust_cube_dimensions(&first_face, &last_face,
                                &first_layer, &last_layer);
        }

        unsigned levels = 1 + last_level - first_level;
        unsigned layers = 1 + last_layer - first_layer;
        unsigned faces  = 1 + last_face  - first_face;
        unsigned num_elements = levels * layers * faces * MAX2(nr_samples, 1);

        if (manual_stride)
                num_elements *= 2;

        return num_elements;
}

/* Conservative estimate of the size of the texture payload a priori.
 * Average case, size equal to the actual size. Worst case, off by 2x (if
 * a manual stride is not needed on a linear texture). Returned value
 * must be greater than or equal to the actual size, so it's safe to use
 * as an allocation amount */

unsigned
panfrost_estimate_texture_payload_size(
                unsigned first_level, unsigned last_level,
                unsigned first_layer, unsigned last_layer,
                unsigned nr_samples,
                enum mali_texture_dimension dim, uint64_t modifier)
{
        /* Assume worst case */
        unsigned manual_stride = (modifier == DRM_FORMAT_MOD_LINEAR);

        unsigned elements = panfrost_texture_num_elements(
                        first_level, last_level,
                        first_layer, last_layer,
                        nr_samples,
                        dim == MALI_TEXTURE_DIMENSION_CUBE, manual_stride);

        return sizeof(mali_ptr) * elements;
}

/* Bifrost requires a tile stride for tiled textures. This stride is computed
 * as (16 * bpp * width) assuming there is at least one tile (width >= 16).
 * Otherwise if height <= 16, the blob puts zero. Interactions with AFBC are
 * currently unknown.
 */

static unsigned
panfrost_nonlinear_stride(uint64_t modifier,
                unsigned bytes_per_pixel,
                unsigned width,
                unsigned height)
{
        if (modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) {
                return (height <= 16) ? 0 : (16 * bytes_per_pixel * ALIGN_POT(width, 16));
        } else {
                unreachable("TODO: AFBC on Bifrost");
        }
}

static void
panfrost_emit_texture_payload(
        mali_ptr *payload,
        const struct util_format_description *desc,
        enum mali_format mali_format,
        enum mali_texture_dimension dim,
        uint64_t modifier,
        unsigned width, unsigned height,
        unsigned first_level, unsigned last_level,
        unsigned first_layer, unsigned last_layer,
        unsigned nr_samples,
        unsigned cube_stride,
        bool manual_stride,
        mali_ptr base,
        struct panfrost_slice *slices)
{
        base |= panfrost_compression_tag(desc, mali_format, modifier);

        /* Inject the addresses in, interleaving array indices, mip levels,
         * cube faces, and strides in that order */

        unsigned first_face  = 0, last_face = 0, face_mult = 1;

        if (dim == MALI_TEXTURE_DIMENSION_CUBE) {
                face_mult = 6;
                panfrost_adjust_cube_dimensions(&first_face, &last_face, &first_layer, &last_layer);
        }

        nr_samples = MAX2(nr_samples, 1);

        unsigned idx = 0;

        for (unsigned w = first_layer; w <= last_layer; ++w) {
                for (unsigned l = first_level; l <= last_level; ++l) {
                        for (unsigned f = first_face; f <= last_face; ++f) {
                                for (unsigned s = 0; s < nr_samples; ++s) {
                                        payload[idx++] = base + panfrost_texture_offset(
                                                        slices, dim == MALI_TEXTURE_DIMENSION_3D,
                                                        cube_stride, l, w * face_mult + f, s);

                                        if (manual_stride) {
                                                payload[idx++] = (modifier == DRM_FORMAT_MOD_LINEAR) ?
                                                        slices[l].stride :
                                                        panfrost_nonlinear_stride(modifier,
                                                                        MAX2(desc->block.bits / 8, 1),
                                                                        u_minify(width, l),
                                                                        u_minify(height, l));
                                        }
                                }
                        }
                }
        }
}

#define MALI_SWIZZLE_R001 \
        (MALI_CHANNEL_RED << 0) | \
        (MALI_CHANNEL_ZERO << 3) | \
        (MALI_CHANNEL_ZERO << 6) | \
        (MALI_CHANNEL_ONE << 9)

#define MALI_SWIZZLE_A001 \
        (MALI_CHANNEL_ALPHA << 0) | \
        (MALI_CHANNEL_ZERO << 3) | \
        (MALI_CHANNEL_ZERO << 6) | \
        (MALI_CHANNEL_ONE << 9)


void
panfrost_new_texture(
        void *out,
        uint16_t width, uint16_t height,
        uint16_t depth, uint16_t array_size,
        enum pipe_format format,
        enum mali_texture_dimension dim,
        uint64_t modifier,
        unsigned first_level, unsigned last_level,
        unsigned first_layer, unsigned last_layer,
        unsigned nr_samples,
        unsigned cube_stride,
        unsigned swizzle,
        mali_ptr base,
        struct panfrost_slice *slices)
{
        const struct util_format_description *desc =
                util_format_description(format);

        unsigned bytes_per_pixel = util_format_get_blocksize(format);

        enum mali_format mali_format = panfrost_pipe_format_table[desc->format].hw;
        assert(mali_format);

        bool manual_stride = (modifier == DRM_FORMAT_MOD_LINEAR)
                && panfrost_needs_explicit_stride(slices, width,
                                first_level, last_level, bytes_per_pixel);

        unsigned format_swizzle = (format == PIPE_FORMAT_X24S8_UINT) ?
                                MALI_SWIZZLE_A001 :
                                (format == PIPE_FORMAT_S8_UINT) ?
                                MALI_SWIZZLE_R001 :
                                panfrost_translate_swizzle_4(desc->swizzle);

        bool srgb = (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB);

        pan_pack(out, MIDGARD_TEXTURE, cfg) {
                cfg.width = u_minify(width, first_level);
                cfg.height = u_minify(height, first_level);
                cfg.depth = u_minify(depth, first_level);
                cfg.array_size = array_size;
                cfg.format = format_swizzle | (mali_format << 12) | (srgb << 20);
                cfg.dimension = dim;
                cfg.texel_ordering = panfrost_modifier_to_layout(modifier);
                cfg.manual_stride = manual_stride;
                cfg.levels = last_level - first_level;
                cfg.swizzle = swizzle;
        };

        panfrost_emit_texture_payload(
                (mali_ptr *) (out + MALI_MIDGARD_TEXTURE_LENGTH),
                desc,
                mali_format,
                dim,
                modifier,
                width, height,
                first_level, last_level,
                first_layer, last_layer,
                nr_samples,
                cube_stride,
                manual_stride,
                base,
                slices);
}

void
panfrost_new_texture_bifrost(
        struct mali_bifrost_texture_packed *out,
        uint16_t width, uint16_t height,
        uint16_t depth, uint16_t array_size,
        enum pipe_format format,
        enum mali_texture_dimension dim,
        uint64_t modifier,
        unsigned first_level, unsigned last_level,
        unsigned first_layer, unsigned last_layer,
        unsigned nr_samples,
        unsigned cube_stride,
        unsigned swizzle,
        mali_ptr base,
        struct panfrost_slice *slices,
        struct panfrost_bo *payload)
{
        const struct util_format_description *desc =
                util_format_description(format);

        enum mali_format mali_format = panfrost_pipe_format_table[desc->format].hw;
        assert(mali_format);

        panfrost_emit_texture_payload(
                (mali_ptr *) payload->cpu,
                desc,
                mali_format,
                dim,
                modifier,
                width, height,
                first_level, last_level,
                first_layer, last_layer,
                nr_samples,
                cube_stride,
                true, /* Stride explicit on Bifrost */
                base,
                slices);

        bool srgb = (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB);

        pan_pack(out, BIFROST_TEXTURE, cfg) {
                cfg.dimension = dim;
                cfg.format = (mali_format << 12) | (srgb << 20);
                cfg.width = u_minify(width, first_level);
                cfg.height = u_minify(height, first_level);
                cfg.swizzle = swizzle;
                cfg.texel_ordering = panfrost_modifier_to_layout(modifier);
                cfg.levels = last_level - first_level;
                cfg.surfaces = payload->gpu;

                /* Use the sampler descriptor for LOD clamping */
                cfg.minimum_lod = 0;
                cfg.maximum_lod = last_level - first_level;
        }
}

/* Computes sizes for checksumming, which is 8 bytes per 16x16 tile.
 * Checksumming is believed to be a CRC variant (CRC64 based on the size?).
 * This feature is also known as "transaction elimination". */

#define CHECKSUM_TILE_WIDTH 16
#define CHECKSUM_TILE_HEIGHT 16
#define CHECKSUM_BYTES_PER_TILE 8

unsigned
panfrost_compute_checksum_size(
        struct panfrost_slice *slice,
        unsigned width,
        unsigned height)
{
        unsigned aligned_width = ALIGN_POT(width, CHECKSUM_TILE_WIDTH);
        unsigned aligned_height = ALIGN_POT(height, CHECKSUM_TILE_HEIGHT);

        unsigned tile_count_x = aligned_width / CHECKSUM_TILE_WIDTH;
        unsigned tile_count_y = aligned_height / CHECKSUM_TILE_HEIGHT;

        slice->checksum_stride = tile_count_x * CHECKSUM_BYTES_PER_TILE;

        return slice->checksum_stride * tile_count_y;
}

unsigned
panfrost_get_layer_stride(struct panfrost_slice *slices, bool is_3d, unsigned cube_stride, unsigned level)
{
        return is_3d ? slices[level].size0 : cube_stride;
}

/* Computes the offset into a texture at a particular level/face. Add to
 * the base address of a texture to get the address to that level/face */

unsigned
panfrost_texture_offset(struct panfrost_slice *slices, bool is_3d, unsigned cube_stride, unsigned level, unsigned face, unsigned sample)
{
        unsigned layer_stride = panfrost_get_layer_stride(slices, is_3d, cube_stride, level);
        return slices[level].offset + (face * layer_stride) + (sample * slices[level].size0);
}