#pragma once

#include <gdk/gdk.h>

#include "gdk/gdkmemoryformatprivate.h"

typedef struct _GskGpuBuffer            GskGpuBuffer;
typedef struct _GskGpuDevice            GskGpuDevice;
typedef struct _GskGpuFrame             GskGpuFrame;
typedef struct _GskGpuImage             GskGpuImage;
typedef struct _GskGpuOp                GskGpuOp;
typedef struct _GskGpuOpClass           GskGpuOpClass;
typedef struct _GskGpuShaderOp          GskGpuShaderOp;
typedef struct _GskGpuShaderOpClass     GskGpuShaderOpClass;

typedef enum {
  GSK_GPU_SAMPLER_DEFAULT,
  GSK_GPU_SAMPLER_REPEAT,
  GSK_GPU_SAMPLER_NEAREST,
  /* add more */
  GSK_GPU_SAMPLER_N_SAMPLERS
} GskGpuSampler;

typedef enum {
  GSK_GPU_SHADER_CLIP_NONE,
  GSK_GPU_SHADER_CLIP_RECT,
  GSK_GPU_SHADER_CLIP_ROUNDED
} GskGpuShaderClip;
