/*
 * Copyright © 2021 Benjamin Otte
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Benjamin Otte <otte@gnome.org>
 */

#include "config.h"

#include "gdkmemoryformatprivate.h"

#include "gdkcolorprofileprivate.h"
#include "gsk/ngl/fp16private.h"

typedef struct _GdkMemoryFormatDescription GdkMemoryFormatDescription;

typedef enum {
  GDK_MEMORY_ALPHA_PREMULTIPLIED,
  GDK_MEMORY_ALPHA_STRAIGHT,
  GDK_MEMORY_ALPHA_OPAQUE
} GdkMemoryAlpha;

#define TYPED_FUNCS(name, T, R, G, B, A, bpp, scale) \
static void \
name ## _to_float (float        *dest, \
                   const guchar *src_data, \
                   gsize         n) \
{ \
  for (gsize i = 0; i < n; i++) \
    { \
      T *src = (T *) (src_data + i * bpp); \
      dest[0] = (float) src[R] / scale; \
      dest[1] = (float) src[G] / scale; \
      dest[2] = (float) src[B] / scale; \
      if (A >= 0) dest[3] = (float) src[A] / scale; else dest[3] = 1.0; \
      dest += 4; \
    } \
} \
\
static void \
name ## _from_float (guchar      *dest_data, \
                     const float *src, \
                     gsize        n) \
{ \
  for (gsize i = 0; i < n; i++) \
    { \
      T *dest = (T *) (dest_data + i * bpp); \
      dest[R] = CLAMP (src[0] * (scale + 1), 0, scale); \
      dest[G] = CLAMP (src[1] * (scale + 1), 0, scale); \
      dest[B] = CLAMP (src[2] * (scale + 1), 0, scale); \
      if (A >= 0) dest[A] = CLAMP (src[3] * (scale + 1), 0, scale); \
      src += 4; \
    } \
}

TYPED_FUNCS (b8g8r8a8_premultiplied, guchar, 2, 1, 0, 3, 4, 255)
TYPED_FUNCS (a8r8g8b8_premultiplied, guchar, 1, 2, 3, 0, 4, 255)
TYPED_FUNCS (r8g8b8a8_premultiplied, guchar, 0, 1, 2, 3, 4, 255)
TYPED_FUNCS (b8g8r8a8, guchar, 2, 1, 0, 3, 4, 255)
TYPED_FUNCS (a8r8g8b8, guchar, 1, 2, 3, 0, 4, 255)
TYPED_FUNCS (r8g8b8a8, guchar, 0, 1, 2, 3, 4, 255)
TYPED_FUNCS (a8b8g8r8, guchar, 3, 2, 1, 0, 4, 255)
TYPED_FUNCS (r8g8b8, guchar, 0, 1, 2, -1, 3, 255)
TYPED_FUNCS (b8g8r8, guchar, 2, 1, 0, -1, 3, 255)
TYPED_FUNCS (r16g16b16, guint16, 0, 1, 2, -1, 6, 65535)
TYPED_FUNCS (r16g16b16a16, guint16, 0, 1, 2, 3, 8, 65535)

static void
r16g16b16_float_to_float (float        *dest,
                          const guchar *src_data,
                          gsize         n)
{
  guint16 *src = (guint16 *) src_data;
  for (gsize i = 0; i < n; i++)
    {
      half_to_float (src, dest, 3);
      dest[3] = 1.0;
      dest += 4;
      src += 3;
    }
}

static void
r16g16b16_float_from_float (guchar      *dest_data,
                            const float *src,
                            gsize        n)
{
  guint16 *dest = (guint16 *) dest_data;
  for (gsize i = 0; i < n; i++)
    {
      float_to_half (src, dest, 3);
      dest += 3;
      src += 4;
    }
}

static void
r16g16b16a16_float_to_float (float        *dest,
                             const guchar *src,
                             gsize         n)
{
  half_to_float ((const guint16 *) src, dest, 4 * n);
}

static void
r16g16b16a16_float_from_float (guchar      *dest,
                               const float *src,
                               gsize        n)
{
  float_to_half (src, (guint16 *) dest, 4 * n);
}

static void
r32g32b32_float_to_float (float        *dest,
                          const guchar *src_data,
                          gsize         n)
{
  float *src = (float *) src_data;
  for (gsize i = 0; i < n; i++)
    {
      dest[0] = src[0];
      dest[1] = src[1];
      dest[2] = src[2];
      dest[3] = 1.0;
      dest += 4;
      src += 3;
    }
}

static void
r32g32b32_float_from_float (guchar      *dest_data,
                            const float *src,
                            gsize        n)
{
  float *dest = (float *) dest_data;
  for (gsize i = 0; i < n; i++)
    {
      dest[0] = src[0];
      dest[1] = src[1];
      dest[2] = src[2];
      dest += 3;
      src += 4;
    }
}

static void
r32g32b32a32_float_to_float (float        *dest,
                             const guchar *src,
                             gsize         n)
{
  memcpy (dest, src, sizeof (float) * n * 4);
}

static void
r32g32b32a32_float_from_float (guchar      *dest,
                               const float *src,
                               gsize        n)
{
  memcpy (dest, src, sizeof (float) * n * 4);
}

struct _GdkMemoryFormatDescription
{
  GdkMemoryAlpha alpha;
  gsize bytes_per_pixel;
  gsize alignment;

  /* no premultiplication going on here */
  void (* to_float) (float *, const guchar*, gsize);
  void (* from_float) (guchar *, const float *, gsize);
};

static const GdkMemoryFormatDescription memory_formats[GDK_MEMORY_N_FORMATS] = {
  [GDK_MEMORY_B8G8R8A8_PREMULTIPLIED] = {
    GDK_MEMORY_ALPHA_PREMULTIPLIED,
    4,
    G_ALIGNOF (guchar),
    b8g8r8a8_premultiplied_to_float,
    b8g8r8a8_premultiplied_from_float,
  },
  [GDK_MEMORY_A8R8G8B8_PREMULTIPLIED] = {
    GDK_MEMORY_ALPHA_PREMULTIPLIED,
    4,
    G_ALIGNOF (guchar),
    a8r8g8b8_premultiplied_to_float,
    a8r8g8b8_premultiplied_from_float,
  },
  [GDK_MEMORY_R8G8B8A8_PREMULTIPLIED] = {
    GDK_MEMORY_ALPHA_PREMULTIPLIED,
    4,
    G_ALIGNOF (guchar),
    r8g8b8a8_premultiplied_to_float,
    r8g8b8a8_premultiplied_from_float,
  },
  [GDK_MEMORY_B8G8R8A8] = {
    GDK_MEMORY_ALPHA_STRAIGHT,
    4,
    G_ALIGNOF (guchar),
    b8g8r8a8_to_float,
    b8g8r8a8_from_float,
  },
  [GDK_MEMORY_A8R8G8B8] = {
    GDK_MEMORY_ALPHA_STRAIGHT,
    4,
    G_ALIGNOF (guchar),
    a8r8g8b8_to_float,
    a8r8g8b8_from_float,
  },
  [GDK_MEMORY_R8G8B8A8] = {
    GDK_MEMORY_ALPHA_STRAIGHT,
    4,
    G_ALIGNOF (guchar),
    r8g8b8a8_to_float,
    r8g8b8a8_from_float,
  },
  [GDK_MEMORY_A8B8G8R8] = {
    GDK_MEMORY_ALPHA_STRAIGHT,
    4,
    G_ALIGNOF (guchar),
    a8b8g8r8_to_float,
    a8b8g8r8_from_float,
  },
  [GDK_MEMORY_R8G8B8] = {
    GDK_MEMORY_ALPHA_OPAQUE,
    3,
    G_ALIGNOF (guchar),
    r8g8b8_to_float,
    r8g8b8_from_float,
  },
  [GDK_MEMORY_B8G8R8] = {
    GDK_MEMORY_ALPHA_OPAQUE,
    3,
    G_ALIGNOF (guchar),
    b8g8r8_to_float,
    b8g8r8_from_float,
  },
  [GDK_MEMORY_R16G16B16] = {
    GDK_MEMORY_ALPHA_OPAQUE,
    6,
    G_ALIGNOF (guint16),
    r16g16b16_to_float,
    r16g16b16_from_float,
  },
  [GDK_MEMORY_R16G16B16A16_PREMULTIPLIED] = {
    GDK_MEMORY_ALPHA_PREMULTIPLIED,
    8,
    G_ALIGNOF (guint16),
    r16g16b16a16_to_float,
    r16g16b16a16_from_float,
  },
  [GDK_MEMORY_R16G16B16_FLOAT] = {
    GDK_MEMORY_ALPHA_OPAQUE,
    6,
    G_ALIGNOF (guint16),
    r16g16b16_float_to_float,
    r16g16b16_float_from_float,
  },
  [GDK_MEMORY_R16G16B16A16_FLOAT_PREMULTIPLIED] = {
    GDK_MEMORY_ALPHA_PREMULTIPLIED,
    8,
    G_ALIGNOF (guint16),
    r16g16b16a16_float_to_float,
    r16g16b16a16_float_from_float,
  },
  [GDK_MEMORY_R32G32B32_FLOAT] = {
    GDK_MEMORY_ALPHA_OPAQUE,
    12,
    G_ALIGNOF (float),
    r32g32b32_float_to_float,
    r32g32b32_float_from_float,
  },
  [GDK_MEMORY_R32G32B32A32_FLOAT_PREMULTIPLIED] = {
    GDK_MEMORY_ALPHA_PREMULTIPLIED,
    16,
    G_ALIGNOF (float),
    r32g32b32a32_float_to_float,
    r32g32b32a32_float_from_float,
  }
};

gsize
gdk_memory_format_bytes_per_pixel (GdkMemoryFormat format)
{
  return memory_formats[format].bytes_per_pixel;
}

gsize
gdk_memory_format_alignment (GdkMemoryFormat format)
{
  return memory_formats[format].alignment;
}

static void
premultiply (float *rgba,
             gsize  n)
{
  for (gsize i = 0; i < n; i++)
    {
      rgba[0] *= rgba[3];
      rgba[1] *= rgba[3];
      rgba[2] *= rgba[3];
      rgba += 4;
    }
}

static void
unpremultiply (float *rgba,
               gsize  n)
{
  for (gsize i = 0; i < n; i++)
    {
      if (rgba[3] > 1/255.0)
        {
          rgba[0] /= rgba[3];
          rgba[1] /= rgba[3];
          rgba[2] /= rgba[3];
        }
      rgba += 4;
    }
}

void
gdk_memory_convert (guchar              *dest_data,
                    gsize                dest_stride,
                    GdkMemoryFormat      dest_format,
                    GdkColorProfile     *dest_profile,
                    const guchar        *src_data,
                    gsize                src_stride,
                    GdkMemoryFormat      src_format,
                    GdkColorProfile     *src_profile,
                    gsize                width,
                    gsize                height)
{
  const GdkMemoryFormatDescription *dest_desc = &memory_formats[dest_format];
  const GdkMemoryFormatDescription *src_desc = &memory_formats[src_format];
  cmsHTRANSFORM transform;
  float *tmp;
  gsize y;

  g_assert (dest_format < GDK_MEMORY_N_FORMATS);
  g_assert (src_format < GDK_MEMORY_N_FORMATS);

  tmp = g_new (float, width * 4);

  if (gdk_color_profile_equal (src_profile, dest_profile))
    {
      transform = NULL;
    }
  else
    {
      transform = cmsCreateTransform (gdk_color_profile_get_lcms_profile (src_profile),
                                      TYPE_RGBA_FLT,
                                      gdk_color_profile_get_lcms_profile (dest_profile),
                                      TYPE_RGBA_FLT,
                                      INTENT_PERCEPTUAL,
                                      cmsFLAGS_COPY_ALPHA);
    }

  for (y = 0; y < height; y++)
    {
      src_desc->to_float (tmp, src_data, width);
      if (transform)
        {
          if (src_desc->alpha == GDK_MEMORY_ALPHA_PREMULTIPLIED)
            unpremultiply (tmp, width);
          cmsDoTransform (transform,
                          tmp,
                          tmp,
                          width);
          if (dest_desc->alpha != GDK_MEMORY_ALPHA_STRAIGHT)
            premultiply (tmp, width);
        }
      else
        {
          if (src_desc->alpha == GDK_MEMORY_ALPHA_PREMULTIPLIED && dest_desc->alpha == GDK_MEMORY_ALPHA_STRAIGHT)
            unpremultiply (tmp, width);
          else if (src_desc->alpha == GDK_MEMORY_ALPHA_STRAIGHT && dest_desc->alpha != GDK_MEMORY_ALPHA_STRAIGHT)
            premultiply (tmp, width);
        }
      dest_desc->from_float (dest_data, tmp, width);
      src_data += src_stride;
      dest_data += dest_stride;
    }

  g_free (tmp);
  g_clear_pointer (&transform, cmsDeleteTransform);
}