/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DrawTargetCairo.h"

#include "SourceSurfaceCairo.h"
#include "PathCairo.h"
#include "HelpersCairo.h"
#include "ScaledFontBase.h"
#include "BorrowedContext.h"
#include "FilterNodeSoftware.h"
#include "mozilla/Scoped.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

#include "cairo.h"
#include "cairo-tee.h"
#include <string.h>

#include "Blur.h"
#include "Logging.h"
#include "Tools.h"

#ifdef CAIRO_HAS_QUARTZ_SURFACE
#include "cairo-quartz.h"
#ifdef MOZ_WIDGET_COCOA
#include <ApplicationServices/ApplicationServices.h>
#endif
#endif

#ifdef CAIRO_HAS_XLIB_SURFACE
#include "cairo-xlib.h"
#include "cairo-xlib-xrender.h"
#endif

#ifdef CAIRO_HAS_WIN32_SURFACE
#include "cairo-win32.h"
#endif

#define PIXMAN_DONT_DEFINE_STDINT
#include "pixman.h"

#include <algorithm>

// 2^23
#define CAIRO_COORD_MAX (Float(0x7fffff))

namespace mozilla {

MOZ_TYPE_SPECIFIC_SCOPED_POINTER_TEMPLATE(ScopedCairoSurface, cairo_surface_t, cairo_surface_destroy);

namespace gfx {

cairo_surface_t *DrawTargetCairo::mDummySurface;

namespace {

// An RAII class to prepare to draw a context and optional path. Saves and
// restores the context on construction/destruction.
class AutoPrepareForDrawing
{
public:
  AutoPrepareForDrawing(DrawTargetCairo* dt, cairo_t* ctx)
    : mCtx(ctx)
  {
    dt->PrepareForDrawing(ctx);
    cairo_save(mCtx);
    MOZ_ASSERT(cairo_status(mCtx) || dt->GetTransform() == GetTransform());
  }

  AutoPrepareForDrawing(DrawTargetCairo* dt, cairo_t* ctx, const Path* path)
    : mCtx(ctx)
  {
    dt->PrepareForDrawing(ctx, path);
    cairo_save(mCtx);
    MOZ_ASSERT(cairo_status(mCtx) || dt->GetTransform() == GetTransform());
  }

  ~AutoPrepareForDrawing()
  {
    cairo_restore(mCtx);
    cairo_status_t status = cairo_status(mCtx);
    if (status) {
      gfxWarning() << "DrawTargetCairo context in error state: " << cairo_status_to_string(status) << "(" << status << ")";
    }
  }

private:
#ifdef DEBUG
  Matrix GetTransform()
  {
    cairo_matrix_t mat;
    cairo_get_matrix(mCtx, &mat);
    return Matrix(mat.xx, mat.yx, mat.xy, mat.yy, mat.x0, mat.y0);
  }
#endif

  cairo_t* mCtx;
};

/* Clamp r to (0,0) (2^23,2^23)
 * these are to be device coordinates.
 *
 * Returns false if the rectangle is completely out of bounds,
 * true otherwise.
 *
 * This function assumes that it will be called with a rectangle being
 * drawn into a surface with an identity transformation matrix; that
 * is, anything above or to the left of (0,0) will be offscreen.
 *
 * First it checks if the rectangle is entirely beyond
 * CAIRO_COORD_MAX; if so, it can't ever appear on the screen --
 * false is returned.
 *
 * Then it shifts any rectangles with x/y < 0 so that x and y are = 0,
 * and adjusts the width and height appropriately.  For example, a
 * rectangle from (0,-5) with dimensions (5,10) will become a
 * rectangle from (0,0) with dimensions (5,5).
 *
 * If after negative x/y adjustment to 0, either the width or height
 * is negative, then the rectangle is completely offscreen, and
 * nothing is drawn -- false is returned.
 *
 * Finally, if x+width or y+height are greater than CAIRO_COORD_MAX,
 * the width and height are clamped such x+width or y+height are equal
 * to CAIRO_COORD_MAX, and true is returned.
 */
static bool
ConditionRect(Rect& r) {
  // if either x or y is way out of bounds;
  // note that we don't handle negative w/h here
  if (r.X() > CAIRO_COORD_MAX || r.Y() > CAIRO_COORD_MAX)
    return false;

  if (r.X() < 0.f) {
    r.width += r.X();
    if (r.width < 0.f)
      return false;
    r.x = 0.f;
  }

  if (r.XMost() > CAIRO_COORD_MAX) {
    r.width = CAIRO_COORD_MAX - r.X();
  }

  if (r.Y() < 0.f) {
    r.height += r.Y();
    if (r.Height() < 0.f)
      return false;

    r.y = 0.f;
  }

  if (r.YMost() > CAIRO_COORD_MAX) {
    r.height = CAIRO_COORD_MAX - r.Y();
  }
  return true;
}

} // end anonymous namespace

static bool
SupportsSelfCopy(cairo_surface_t* surface)
{
  switch (cairo_surface_get_type(surface))
  {
#ifdef CAIRO_HAS_QUARTZ_SURFACE
    case CAIRO_SURFACE_TYPE_QUARTZ:
      return true;
#endif
#ifdef CAIRO_HAS_WIN32_SURFACE
    case CAIRO_SURFACE_TYPE_WIN32:
    case CAIRO_SURFACE_TYPE_WIN32_PRINTING:
      return true;
#endif
    default:
      return false;
  }
}

static bool
PatternIsCompatible(const Pattern& aPattern)
{
  switch (aPattern.GetType())
  {
    case PatternType::LINEAR_GRADIENT:
    {
      const LinearGradientPattern& pattern = static_cast<const LinearGradientPattern&>(aPattern);
      return pattern.mStops->GetBackendType() == BackendType::CAIRO;
    }
    case PatternType::RADIAL_GRADIENT:
    {
      const RadialGradientPattern& pattern = static_cast<const RadialGradientPattern&>(aPattern);
      return pattern.mStops->GetBackendType() == BackendType::CAIRO;
    }
    default:
      return true;
  }
}

static cairo_user_data_key_t surfaceDataKey;

void
ReleaseData(void* aData)
{
  DataSourceSurface *data = static_cast<DataSourceSurface*>(aData);
  data->Unmap();
  data->Release();
}

cairo_surface_t*
CopyToImageSurface(unsigned char *aData,
                   const IntRect &aRect,
                   int32_t aStride,
                   SurfaceFormat aFormat)
{
  MOZ_ASSERT(aData);

  cairo_surface_t* surf = cairo_image_surface_create(GfxFormatToCairoFormat(aFormat),
                                                     aRect.width,
                                                     aRect.height);
  // In certain scenarios, requesting larger than 8k image fails.  Bug 803568
  // covers the details of how to run into it, but the full detailed
  // investigation hasn't been done to determine the underlying cause.  We
  // will just handle the failure to allocate the surface to avoid a crash.
  if (cairo_surface_status(surf)) {
    gfxWarning() << "Invalid surface DTC " << cairo_surface_status(surf);
    return nullptr;
  }

  unsigned char* surfData = cairo_image_surface_get_data(surf);
  int surfStride = cairo_image_surface_get_stride(surf);
  int32_t pixelWidth = BytesPerPixel(aFormat);

  unsigned char* source = aData +
                          aRect.y * aStride +
                          aRect.x * pixelWidth;

  MOZ_ASSERT(aStride >= aRect.width * pixelWidth);
  for (int32_t y = 0; y < aRect.height; ++y) {
    memcpy(surfData + y * surfStride,
           source + y * aStride,
           aRect.width * pixelWidth);
  }
  cairo_surface_mark_dirty(surf);
  return surf;
}

/**
 * If aSurface can be represented as a surface of type
 * CAIRO_SURFACE_TYPE_IMAGE then returns that surface. Does
 * not add a reference.
 */
cairo_surface_t* GetAsImageSurface(cairo_surface_t* aSurface)
{
  if (cairo_surface_get_type(aSurface) == CAIRO_SURFACE_TYPE_IMAGE) {
    return aSurface;
#ifdef CAIRO_HAS_WIN32_SURFACE
  } else if (cairo_surface_get_type(aSurface) == CAIRO_SURFACE_TYPE_WIN32) {
    return cairo_win32_surface_get_image(aSurface);
#endif
  }

  return nullptr;
}

cairo_surface_t* CreateSubImageForData(unsigned char* aData,
                                       const IntRect& aRect,
                                       int aStride,
                                       SurfaceFormat aFormat)
{
  if (!aData) {
    gfxWarning() << "DrawTargetCairo.CreateSubImageForData null aData";
    return nullptr;
  }
  unsigned char *data = aData +
                        aRect.y * aStride +
                        aRect.x * BytesPerPixel(aFormat);

  cairo_surface_t *image =
    cairo_image_surface_create_for_data(data,
                                        GfxFormatToCairoFormat(aFormat),
                                        aRect.width,
                                        aRect.height,
                                        aStride);
  cairo_surface_set_device_offset(image, -aRect.x, -aRect.y);
  return image;
}

/**
 * Returns a referenced cairo_surface_t representing the
 * sub-image specified by aSubImage.
 */
cairo_surface_t* ExtractSubImage(cairo_surface_t* aSurface,
                                 const IntRect& aSubImage,
                                 SurfaceFormat aFormat)
{
  // No need to worry about retaining a reference to the original
  // surface since the only caller of this function guarantees
  // that aSurface will stay alive as long as the result

  cairo_surface_t* image = GetAsImageSurface(aSurface);
  if (image) {
    image = CreateSubImageForData(cairo_image_surface_get_data(image),
                                  aSubImage,
                                  cairo_image_surface_get_stride(image),
                                  aFormat);
    return image;
  }

  cairo_surface_t* similar =
    cairo_surface_create_similar(aSurface,
                                 cairo_surface_get_content(aSurface),
                                 aSubImage.width, aSubImage.height);

  cairo_t* ctx = cairo_create(similar);
  cairo_set_operator(ctx, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(ctx, aSurface, -aSubImage.x, -aSubImage.y);
  cairo_paint(ctx);
  cairo_destroy(ctx);

  cairo_surface_set_device_offset(similar, -aSubImage.x, -aSubImage.y);
  return similar;
}

/**
 * Returns cairo surface for the given SourceSurface.
 * If possible, it will use the cairo_surface associated with aSurface,
 * otherwise, it will create a new cairo_surface.
 * In either case, the caller must call cairo_surface_destroy on the
 * result when it is done with it.
 */
cairo_surface_t*
GetCairoSurfaceForSourceSurface(SourceSurface *aSurface,
                                bool aExistingOnly = false,
                                const IntRect& aSubImage = IntRect())
{
  if (!aSurface) {
    return nullptr;
  }

  IntRect subimage = IntRect(IntPoint(), aSurface->GetSize());
  if (!aSubImage.IsEmpty()) {
    MOZ_ASSERT(!aExistingOnly);
    MOZ_ASSERT(subimage.Contains(aSubImage));
    subimage = aSubImage;
  }

  if (aSurface->GetType() == SurfaceType::CAIRO) {
    cairo_surface_t* surf = static_cast<SourceSurfaceCairo*>(aSurface)->GetSurface();
    if (aSubImage.IsEmpty()) {
      cairo_surface_reference(surf);
    } else {
      surf = ExtractSubImage(surf, subimage, aSurface->GetFormat());
    }
    return surf;
  }

  if (aSurface->GetType() == SurfaceType::CAIRO_IMAGE) {
    cairo_surface_t* surf =
      static_cast<const DataSourceSurfaceCairo*>(aSurface)->GetSurface();
    if (aSubImage.IsEmpty()) {
      cairo_surface_reference(surf);
    } else {
      surf = ExtractSubImage(surf, subimage, aSurface->GetFormat());
    }
    return surf;
  }

  if (aExistingOnly) {
    return nullptr;
  }

  RefPtr<DataSourceSurface> data = aSurface->GetDataSurface();
  if (!data) {
    return nullptr;
  }

  DataSourceSurface::MappedSurface map;
  if (!data->Map(DataSourceSurface::READ, &map)) {
    return nullptr;
  }

  cairo_surface_t* surf =
    CreateSubImageForData(map.mData, subimage,
                          map.mStride, data->GetFormat());

  // In certain scenarios, requesting larger than 8k image fails.  Bug 803568
  // covers the details of how to run into it, but the full detailed
  // investigation hasn't been done to determine the underlying cause.  We
  // will just handle the failure to allocate the surface to avoid a crash.
  if (!surf || cairo_surface_status(surf)) {
    if (surf && (cairo_surface_status(surf) == CAIRO_STATUS_INVALID_STRIDE)) {
      // If we failed because of an invalid stride then copy into
      // a new surface with a stride that cairo chooses. No need to
      // set user data since we're not dependent on the original
      // data.
      cairo_surface_t* result =
        CopyToImageSurface(map.mData,
                           subimage,
                           map.mStride,
                           data->GetFormat());
      data->Unmap();
      return result;
    }
    data->Unmap();
    return nullptr;
  }

  cairo_surface_set_user_data(surf,
                              &surfaceDataKey,
                              data.forget().take(),
                              ReleaseData);
  return surf;
}

// An RAII class to temporarily clear any device offset set
// on a surface. Note that this does not take a reference to the
// surface.
class AutoClearDeviceOffset
{
public:
  explicit AutoClearDeviceOffset(SourceSurface* aSurface)
    : mSurface(nullptr)
    , mX(0)
    , mY(0)
  {
    Init(aSurface);
  }

  explicit AutoClearDeviceOffset(const Pattern& aPattern)
    : mSurface(nullptr)
  {
    if (aPattern.GetType() == PatternType::SURFACE) {
      const SurfacePattern& pattern = static_cast<const SurfacePattern&>(aPattern);
      Init(pattern.mSurface);
    }
  }

  ~AutoClearDeviceOffset()
  {
    if (mSurface) {
      cairo_surface_set_device_offset(mSurface, mX, mY);
    }
  }

private:
  void Init(SourceSurface* aSurface)
  {
    cairo_surface_t* surface = GetCairoSurfaceForSourceSurface(aSurface, true);
    if (surface) {
      Init(surface);
      cairo_surface_destroy(surface);
    }
  }

  void Init(cairo_surface_t *aSurface)
  {
    mSurface = aSurface;
    cairo_surface_get_device_offset(mSurface, &mX, &mY);
    cairo_surface_set_device_offset(mSurface, 0, 0);
  }

  cairo_surface_t* mSurface;
  double mX;
  double mY;
};

static inline void
CairoPatternAddGradientStop(cairo_pattern_t* aPattern,
                            const GradientStop &aStop,
                            Float aNudge = 0)
{
  cairo_pattern_add_color_stop_rgba(aPattern, aStop.offset + aNudge,
                                    aStop.color.r, aStop.color.g, aStop.color.b,
                                    aStop.color.a);

}

// Never returns nullptr. As such, you must always pass in Cairo-compatible
// patterns, most notably gradients with a GradientStopCairo.
// The pattern returned must have cairo_pattern_destroy() called on it by the
// caller.
// As the cairo_pattern_t returned may depend on the Pattern passed in, the
// lifetime of the cairo_pattern_t returned must not exceed the lifetime of the
// Pattern passed in.
static cairo_pattern_t*
GfxPatternToCairoPattern(const Pattern& aPattern,
                         Float aAlpha,
                         const Matrix& aTransform)
{
  cairo_pattern_t* pat;
  const Matrix* matrix = nullptr;

  switch (aPattern.GetType())
  {
    case PatternType::COLOR:
    {
      Color color = static_cast<const ColorPattern&>(aPattern).mColor;
      pat = cairo_pattern_create_rgba(color.r, color.g, color.b, color.a * aAlpha);
      break;
    }

    case PatternType::SURFACE:
    {
      const SurfacePattern& pattern = static_cast<const SurfacePattern&>(aPattern);
      cairo_surface_t* surf = GetCairoSurfaceForSourceSurface(pattern.mSurface,
                                                              false,
                                                              pattern.mSamplingRect);
      if (!surf)
        return nullptr;

      pat = cairo_pattern_create_for_surface(surf);

      matrix = &pattern.mMatrix;

      cairo_pattern_set_filter(pat, GfxSamplingFilterToCairoFilter(pattern.mSamplingFilter));
      cairo_pattern_set_extend(pat, GfxExtendToCairoExtend(pattern.mExtendMode));

      cairo_surface_destroy(surf);
      break;
    }
    case PatternType::LINEAR_GRADIENT:
    {
      const LinearGradientPattern& pattern = static_cast<const LinearGradientPattern&>(aPattern);

      pat = cairo_pattern_create_linear(pattern.mBegin.x, pattern.mBegin.y,
                                        pattern.mEnd.x, pattern.mEnd.y);

      MOZ_ASSERT(pattern.mStops->GetBackendType() == BackendType::CAIRO);
      GradientStopsCairo* cairoStops = static_cast<GradientStopsCairo*>(pattern.mStops.get());
      cairo_pattern_set_extend(pat, GfxExtendToCairoExtend(cairoStops->GetExtendMode()));

      matrix = &pattern.mMatrix;

      const std::vector<GradientStop>& stops = cairoStops->GetStops();
      for (size_t i = 0; i < stops.size(); ++i) {
        CairoPatternAddGradientStop(pat, stops[i]);
      }

      break;
    }
    case PatternType::RADIAL_GRADIENT:
    {
      const RadialGradientPattern& pattern = static_cast<const RadialGradientPattern&>(aPattern);

      pat = cairo_pattern_create_radial(pattern.mCenter1.x, pattern.mCenter1.y, pattern.mRadius1,
                                        pattern.mCenter2.x, pattern.mCenter2.y, pattern.mRadius2);

      MOZ_ASSERT(pattern.mStops->GetBackendType() == BackendType::CAIRO);
      GradientStopsCairo* cairoStops = static_cast<GradientStopsCairo*>(pattern.mStops.get());
      cairo_pattern_set_extend(pat, GfxExtendToCairoExtend(cairoStops->GetExtendMode()));

      matrix = &pattern.mMatrix;

      const std::vector<GradientStop>& stops = cairoStops->GetStops();
      for (size_t i = 0; i < stops.size(); ++i) {
        CairoPatternAddGradientStop(pat, stops[i]);
      }

      break;
    }
    default:
    {
      // We should support all pattern types!
      MOZ_ASSERT(false);
    }
  }

  // The pattern matrix is a matrix that transforms the pattern into user
  // space. Cairo takes a matrix that converts from user space to pattern
  // space. Cairo therefore needs the inverse.
  if (matrix) {
    cairo_matrix_t mat;
    GfxMatrixToCairoMatrix(*matrix, mat);
    cairo_matrix_invert(&mat);
    cairo_pattern_set_matrix(pat, &mat);
  }

  return pat;
}

static bool
NeedIntermediateSurface(const Pattern& aPattern, const DrawOptions& aOptions)
{
  // We pre-multiply colours' alpha by the global alpha, so we don't need to
  // use an intermediate surface for them.
  if (aPattern.GetType() == PatternType::COLOR)
    return false;

  if (aOptions.mAlpha == 1.0)
    return false;

  return true;
}

DrawTargetCairo::DrawTargetCairo()
  : mContext(nullptr)
  , mSurface(nullptr)
  , mTransformSingular(false)
  , mLockedBits(nullptr)
  , mFontOptions(nullptr)
{
}

DrawTargetCairo::~DrawTargetCairo()
{
  cairo_destroy(mContext);
  if (mSurface) {
    cairo_surface_destroy(mSurface);
    mSurface = nullptr;
  }
  if (mFontOptions) {
    cairo_font_options_destroy(mFontOptions);
    mFontOptions = nullptr;
  }
  MOZ_ASSERT(!mLockedBits);
}

bool
DrawTargetCairo::IsValid() const
{
  return mSurface && !cairo_surface_status(mSurface) &&
         mContext /*&& !cairo_surface_status(cairo_get_group_target(mContext))*/;
}

DrawTargetType
DrawTargetCairo::GetType() const
{
  if (mContext) {
    cairo_surface_type_t type = cairo_surface_get_type(mSurface);
    if (type == CAIRO_SURFACE_TYPE_TEE) {
      type = cairo_surface_get_type(cairo_tee_surface_index(mSurface, 0));
      MOZ_ASSERT(type != CAIRO_SURFACE_TYPE_TEE, "C'mon!");
      MOZ_ASSERT(type == cairo_surface_get_type(cairo_tee_surface_index(mSurface, 1)),
                 "What should we do here?");
    }
    switch (type) {
    case CAIRO_SURFACE_TYPE_PDF:
    case CAIRO_SURFACE_TYPE_PS:
    case CAIRO_SURFACE_TYPE_SVG:
    case CAIRO_SURFACE_TYPE_WIN32_PRINTING:
    case CAIRO_SURFACE_TYPE_XML:
      return DrawTargetType::VECTOR;

    case CAIRO_SURFACE_TYPE_VG:
    case CAIRO_SURFACE_TYPE_GL:
    case CAIRO_SURFACE_TYPE_GLITZ:
    case CAIRO_SURFACE_TYPE_QUARTZ:
    case CAIRO_SURFACE_TYPE_DIRECTFB:
      return DrawTargetType::HARDWARE_RASTER;

    case CAIRO_SURFACE_TYPE_SKIA:
    case CAIRO_SURFACE_TYPE_QT:
      MOZ_FALLTHROUGH_ASSERT("Can't determine actual DrawTargetType for DrawTargetCairo - assuming SOFTWARE_RASTER");
    case CAIRO_SURFACE_TYPE_IMAGE:
    case CAIRO_SURFACE_TYPE_XLIB:
    case CAIRO_SURFACE_TYPE_XCB:
    case CAIRO_SURFACE_TYPE_WIN32:
    case CAIRO_SURFACE_TYPE_BEOS:
    case CAIRO_SURFACE_TYPE_OS2:
    case CAIRO_SURFACE_TYPE_QUARTZ_IMAGE:
    case CAIRO_SURFACE_TYPE_SCRIPT:
    case CAIRO_SURFACE_TYPE_RECORDING:
    case CAIRO_SURFACE_TYPE_DRM:
    case CAIRO_SURFACE_TYPE_SUBSURFACE:
    case CAIRO_SURFACE_TYPE_TEE: // included to silence warning about unhandled enum value
      return DrawTargetType::SOFTWARE_RASTER;
    default:
      MOZ_CRASH("GFX: Unsupported cairo surface type");
    }
  }
  MOZ_ASSERT(false, "Could not determine DrawTargetType for DrawTargetCairo");
  return DrawTargetType::SOFTWARE_RASTER;
}

IntSize
DrawTargetCairo::GetSize()
{
  return mSize;
}

SurfaceFormat
GfxFormatForCairoSurface(cairo_surface_t* surface)
{
  cairo_surface_type_t type = cairo_surface_get_type(surface);
  if (type == CAIRO_SURFACE_TYPE_IMAGE) {
    return CairoFormatToGfxFormat(cairo_image_surface_get_format(surface));
  }
#ifdef CAIRO_HAS_XLIB_SURFACE
  // xlib is currently the only Cairo backend that creates 16bpp surfaces
  if (type == CAIRO_SURFACE_TYPE_XLIB &&
      cairo_xlib_surface_get_depth(surface) == 16) {
    return SurfaceFormat::R5G6B5_UINT16;
  }
#endif
  return CairoContentToGfxFormat(cairo_surface_get_content(surface));
}

already_AddRefed<SourceSurface>
DrawTargetCairo::Snapshot()
{
  if (!IsValid()) {
    gfxCriticalNote << "DrawTargetCairo::Snapshot with bad surface " << cairo_surface_status(mSurface);
    return nullptr;
  }
  if (mSnapshot) {
    RefPtr<SourceSurface> snapshot(mSnapshot);
    return snapshot.forget();
  }

  IntSize size = GetSize();

  mSnapshot = new SourceSurfaceCairo(mSurface,
                                     size,
                                     GfxFormatForCairoSurface(mSurface),
                                     this);
  RefPtr<SourceSurface> snapshot(mSnapshot);
  return snapshot.forget();
}

bool
DrawTargetCairo::LockBits(uint8_t** aData, IntSize* aSize,
                          int32_t* aStride, SurfaceFormat* aFormat,
                          IntPoint* aOrigin)
{
  cairo_surface_t* target = cairo_get_group_target(mContext);
  cairo_surface_t* surf = target;
#ifdef CAIRO_HAS_WIN32_SURFACE
  if (cairo_surface_get_type(surf) == CAIRO_SURFACE_TYPE_WIN32) {
    cairo_surface_t* imgsurf = cairo_win32_surface_get_image(surf);
    if (imgsurf) {
      surf = imgsurf;
    }
  }
#endif
  if (cairo_surface_get_type(surf) == CAIRO_SURFACE_TYPE_IMAGE &&
      cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
    PointDouble offset;
    cairo_surface_get_device_offset(target, &offset.x, &offset.y);
    // verify the device offset can be converted to integers suitable for a bounds rect
    IntPoint origin(int32_t(-offset.x), int32_t(-offset.y));
    if (-PointDouble(origin) != offset ||
        (!aOrigin && origin != IntPoint())) {
      return false;
    }

    WillChange();
    Flush();

    mLockedBits = cairo_image_surface_get_data(surf);
    *aData = mLockedBits;
    *aSize = IntSize(cairo_image_surface_get_width(surf),
                     cairo_image_surface_get_height(surf));
    *aStride = cairo_image_surface_get_stride(surf);
    *aFormat = CairoFormatToGfxFormat(cairo_image_surface_get_format(surf));
    if (aOrigin) {
      *aOrigin = origin;
    }
    return true;
  }

  return false;
}

void
DrawTargetCairo::ReleaseBits(uint8_t* aData)
{
  MOZ_ASSERT(mLockedBits == aData);
  mLockedBits = nullptr;
  cairo_surface_t* surf = cairo_get_group_target(mContext);
#ifdef CAIRO_HAS_WIN32_SURFACE
  if (cairo_surface_get_type(surf) == CAIRO_SURFACE_TYPE_WIN32) {
    cairo_surface_t* imgsurf = cairo_win32_surface_get_image(surf);
    if (imgsurf) {
      cairo_surface_mark_dirty(imgsurf);
    }
  }
#endif
  cairo_surface_mark_dirty(surf);
}

void
DrawTargetCairo::Flush()
{
  cairo_surface_t* surf = cairo_get_group_target(mContext);
  cairo_surface_flush(surf);
}

void
DrawTargetCairo::PrepareForDrawing(cairo_t* aContext, const Path* aPath /* = nullptr */)
{
  WillChange(aPath);
}

cairo_surface_t*
DrawTargetCairo::GetDummySurface()
{
  if (mDummySurface) {
    return mDummySurface;
  }

  mDummySurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);

  return mDummySurface;
}

static void
PaintWithAlpha(cairo_t* aContext, const DrawOptions& aOptions)
{
  if (aOptions.mCompositionOp == CompositionOp::OP_SOURCE) {
    // Cairo treats the source operator like a lerp when alpha is < 1.
    // Approximate the desired operator by: out = 0; out += src*alpha;
    if (aOptions.mAlpha == 1) {
      cairo_set_operator(aContext, CAIRO_OPERATOR_SOURCE);
      cairo_paint(aContext);
    } else {
      cairo_set_operator(aContext, CAIRO_OPERATOR_CLEAR);
      cairo_paint(aContext);
      cairo_set_operator(aContext, CAIRO_OPERATOR_ADD);
      cairo_paint_with_alpha(aContext, aOptions.mAlpha);
    }
  } else {
    cairo_set_operator(aContext, GfxOpToCairoOp(aOptions.mCompositionOp));
    cairo_paint_with_alpha(aContext, aOptions.mAlpha);
  }
}

void
DrawTargetCairo::DrawSurface(SourceSurface *aSurface,
                             const Rect &aDest,
                             const Rect &aSource,
                             const DrawSurfaceOptions &aSurfOptions,
                             const DrawOptions &aOptions)
{
  if (mTransformSingular || aDest.IsEmpty()) {
    return;
  }

  if (!IsValid() || !aSurface) {
    gfxCriticalNote << "DrawSurface with bad surface " << cairo_surface_status(cairo_get_group_target(mContext));
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);
  AutoClearDeviceOffset clear(aSurface);

  float sx = aSource.Width() / aDest.Width();
  float sy = aSource.Height() / aDest.Height();

  cairo_matrix_t src_mat;
  cairo_matrix_init_translate(&src_mat, aSource.X(), aSource.Y());
  cairo_matrix_scale(&src_mat, sx, sy);

  cairo_surface_t* surf = GetCairoSurfaceForSourceSurface(aSurface);
  if (!surf) {
    gfxWarning() << "Failed to create cairo surface for DrawTargetCairo::DrawSurface";
    return;
  }
  cairo_pattern_t* pat = cairo_pattern_create_for_surface(surf);
  cairo_surface_destroy(surf);

  cairo_pattern_set_matrix(pat, &src_mat);
  cairo_pattern_set_filter(pat, GfxSamplingFilterToCairoFilter(aSurfOptions.mSamplingFilter));
  cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);

  cairo_set_antialias(mContext, GfxAntialiasToCairoAntialias(aOptions.mAntialiasMode));

  // If the destination rect covers the entire clipped area, then unbounded and bounded
  // operations are identical, and we don't need to push a group.
  bool needsGroup = !IsOperatorBoundByMask(aOptions.mCompositionOp) &&
                    !aDest.Contains(GetUserSpaceClip());

  cairo_translate(mContext, aDest.X(), aDest.Y());

  if (needsGroup) {
    cairo_push_group(mContext);
      cairo_new_path(mContext);
      cairo_rectangle(mContext, 0, 0, aDest.Width(), aDest.Height());
      cairo_set_source(mContext, pat);
      cairo_fill(mContext);
    cairo_pop_group_to_source(mContext);
  } else {
    cairo_new_path(mContext);
    cairo_rectangle(mContext, 0, 0, aDest.Width(), aDest.Height());
    cairo_clip(mContext);
    cairo_set_source(mContext, pat);
  }

  PaintWithAlpha(mContext, aOptions);

  cairo_pattern_destroy(pat);
}

void
DrawTargetCairo::DrawFilter(FilterNode *aNode,
                            const Rect &aSourceRect,
                            const Point &aDestPoint,
                            const DrawOptions &aOptions)
{
  FilterNodeSoftware* filter = static_cast<FilterNodeSoftware*>(aNode);
  filter->Draw(this, aSourceRect, aDestPoint, aOptions);
}

void
DrawTargetCairo::DrawSurfaceWithShadow(SourceSurface *aSurface,
                                       const Point &aDest,
                                       const Color &aColor,
                                       const Point &aOffset,
                                       Float aSigma,
                                       CompositionOp aOperator)
{
  if (aSurface->GetType() != SurfaceType::CAIRO) {
    return;
  }

  AutoClearDeviceOffset clear(aSurface);

  Float width = Float(aSurface->GetSize().width);
  Float height = Float(aSurface->GetSize().height);

  SourceSurfaceCairo* source = static_cast<SourceSurfaceCairo*>(aSurface);
  cairo_surface_t* sourcesurf = source->GetSurface();
  cairo_surface_t* blursurf;
  cairo_surface_t* surf;

  // We only use the A8 surface for blurred shadows. Unblurred shadows can just
  // use the RGBA surface directly.
  if (cairo_surface_get_type(sourcesurf) == CAIRO_SURFACE_TYPE_TEE) {
    blursurf = cairo_tee_surface_index(sourcesurf, 0);
    surf = cairo_tee_surface_index(sourcesurf, 1);
  } else {
    blursurf = sourcesurf;
    surf = sourcesurf;
  }

  if (aSigma != 0.0f) {
    MOZ_ASSERT(cairo_surface_get_type(blursurf) == CAIRO_SURFACE_TYPE_IMAGE);
    Rect extents(0, 0, width, height);
    AlphaBoxBlur blur(extents,
                      cairo_image_surface_get_stride(blursurf),
                      aSigma, aSigma);
    blur.Blur(cairo_image_surface_get_data(blursurf));
  }

  WillChange();
  ClearSurfaceForUnboundedSource(aOperator);

  cairo_save(mContext);
  cairo_set_operator(mContext, GfxOpToCairoOp(aOperator));
  cairo_identity_matrix(mContext);
  cairo_translate(mContext, aDest.x, aDest.y);

  bool needsGroup = !IsOperatorBoundByMask(aOperator);
  if (needsGroup) {
    cairo_push_group(mContext);
  }

  cairo_set_source_rgba(mContext, aColor.r, aColor.g, aColor.b, aColor.a);
  cairo_mask_surface(mContext, blursurf, aOffset.x, aOffset.y);

  if (blursurf != surf ||
      aSurface->GetFormat() != SurfaceFormat::A8) {
    // Now that the shadow has been drawn, we can draw the surface on top.
    cairo_set_source_surface(mContext, surf, 0, 0);
    cairo_new_path(mContext);
    cairo_rectangle(mContext, 0, 0, width, height);
    cairo_fill(mContext);
  }

  if (needsGroup) {
    cairo_pop_group_to_source(mContext);
    cairo_paint(mContext);
  }

  cairo_restore(mContext);
}

void
DrawTargetCairo::DrawPattern(const Pattern& aPattern,
                             const StrokeOptions& aStrokeOptions,
                             const DrawOptions& aOptions,
                             DrawPatternType aDrawType,
                             bool aPathBoundsClip)
{
  if (!PatternIsCompatible(aPattern)) {
    return;
  }

  AutoClearDeviceOffset clear(aPattern);

  cairo_pattern_t* pat = GfxPatternToCairoPattern(aPattern, aOptions.mAlpha, GetTransform());
  if (!pat) {
    return;
  }
  if (cairo_pattern_status(pat)) {
    cairo_pattern_destroy(pat);
    gfxWarning() << "Invalid pattern";
    return;
  }

  cairo_set_source(mContext, pat);

  cairo_set_antialias(mContext, GfxAntialiasToCairoAntialias(aOptions.mAntialiasMode));

  if (NeedIntermediateSurface(aPattern, aOptions) ||
      (!IsOperatorBoundByMask(aOptions.mCompositionOp) && !aPathBoundsClip)) {
    cairo_push_group_with_content(mContext, CAIRO_CONTENT_COLOR_ALPHA);

    // Don't want operators to be applied twice
    cairo_set_operator(mContext, CAIRO_OPERATOR_OVER);

    if (aDrawType == DRAW_STROKE) {
      SetCairoStrokeOptions(mContext, aStrokeOptions);
      cairo_stroke_preserve(mContext);
    } else {
      cairo_fill_preserve(mContext);
    }

    cairo_pop_group_to_source(mContext);

    // Now draw the content using the desired operator
    PaintWithAlpha(mContext, aOptions);
  } else {
    cairo_set_operator(mContext, GfxOpToCairoOp(aOptions.mCompositionOp));

    if (aDrawType == DRAW_STROKE) {
      SetCairoStrokeOptions(mContext, aStrokeOptions);
      cairo_stroke_preserve(mContext);
    } else {
      cairo_fill_preserve(mContext);
    }
  }

  cairo_pattern_destroy(pat);
}

void
DrawTargetCairo::FillRect(const Rect &aRect,
                          const Pattern &aPattern,
                          const DrawOptions &aOptions)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);

  bool restoreTransform = false;
  Matrix mat;
  Rect r = aRect;

  /* Clamp coordinates to work around a design bug in cairo */
  if (r.width > CAIRO_COORD_MAX ||
      r.height > CAIRO_COORD_MAX ||
      r.x < -CAIRO_COORD_MAX ||
      r.x > CAIRO_COORD_MAX ||
      r.y < -CAIRO_COORD_MAX ||
      r.y > CAIRO_COORD_MAX)
  {
    if (!mat.IsRectilinear()) {
      gfxWarning() << "DrawTargetCairo::FillRect() misdrawing huge Rect "
                      "with non-rectilinear transform";
    }

    mat = GetTransform();
    r = mat.TransformBounds(r);

    if (!ConditionRect(r)) {
      gfxWarning() << "Ignoring DrawTargetCairo::FillRect() call with "
                      "out-of-bounds Rect";
      return;
    }

    restoreTransform = true;
    SetTransform(Matrix());
  }

  cairo_new_path(mContext);
  cairo_rectangle(mContext, r.x, r.y, r.Width(), r.Height());

  bool pathBoundsClip = false;

  if (r.Contains(GetUserSpaceClip())) {
    pathBoundsClip = true;
  }

  DrawPattern(aPattern, StrokeOptions(), aOptions, DRAW_FILL, pathBoundsClip);

  if (restoreTransform) {
    SetTransform(mat);
  }
}

void
DrawTargetCairo::CopySurfaceInternal(cairo_surface_t* aSurface,
                                     const IntRect &aSource,
                                     const IntPoint &aDest)
{
  if (cairo_surface_status(aSurface)) {
    gfxWarning() << "Invalid surface" << cairo_surface_status(aSurface);
    return;
  }

  cairo_identity_matrix(mContext);

  cairo_set_source_surface(mContext, aSurface, aDest.x - aSource.x, aDest.y - aSource.y);
  cairo_set_operator(mContext, CAIRO_OPERATOR_SOURCE);
  cairo_set_antialias(mContext, CAIRO_ANTIALIAS_NONE);

  cairo_reset_clip(mContext);
  cairo_new_path(mContext);
  cairo_rectangle(mContext, aDest.x, aDest.y, aSource.width, aSource.height);
  cairo_fill(mContext);
}

void
DrawTargetCairo::CopySurface(SourceSurface *aSurface,
                             const IntRect &aSource,
                             const IntPoint &aDest)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);
  AutoClearDeviceOffset clear(aSurface);

  if (!aSurface) {
    gfxWarning() << "Unsupported surface type specified";
    return;
  }

  cairo_surface_t* surf = GetCairoSurfaceForSourceSurface(aSurface);
  if (!surf) {
    gfxWarning() << "Unsupported surface type specified";
    return;
  }

  CopySurfaceInternal(surf, aSource, aDest);
  cairo_surface_destroy(surf);
}

void
DrawTargetCairo::CopyRect(const IntRect &aSource,
                          const IntPoint &aDest)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);

  IntRect source = aSource;
  cairo_surface_t* surf = mSurface;

  if (!SupportsSelfCopy(mSurface) &&
      aDest.y >= aSource.y &&
      aDest.y < aSource.YMost()) {
    cairo_surface_t* similar = cairo_surface_create_similar(mSurface,
                                                            GfxFormatToCairoContent(GetFormat()),
                                                            aSource.width, aSource.height);
    cairo_t* ctx = cairo_create(similar);
    cairo_set_operator(ctx, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(ctx, surf, -aSource.x, -aSource.y);
    cairo_paint(ctx);
    cairo_destroy(ctx);

    source.x = 0;
    source.y = 0;
    surf = similar;
  }

  CopySurfaceInternal(surf, source, aDest);

  if (surf != mSurface) {
    cairo_surface_destroy(surf);
  }
}

void
DrawTargetCairo::ClearRect(const Rect& aRect)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);

  if (!mContext || aRect.Width() < 0 || aRect.Height() < 0 ||
      !IsFinite(aRect.X()) || !IsFinite(aRect.Width()) ||
      !IsFinite(aRect.Y()) || !IsFinite(aRect.Height())) {
    gfxCriticalNote << "ClearRect with invalid argument " << gfx::hexa(mContext) << " with " << aRect.Width() << "x" << aRect.Height() << " [" << aRect.X() << ", " << aRect.Y() << "]";
  }

  cairo_set_antialias(mContext, CAIRO_ANTIALIAS_NONE);
  cairo_new_path(mContext);
  cairo_set_operator(mContext, CAIRO_OPERATOR_CLEAR);
  cairo_rectangle(mContext, aRect.X(), aRect.Y(),
                  aRect.Width(), aRect.Height());
  cairo_fill(mContext);
}

void
DrawTargetCairo::StrokeRect(const Rect &aRect,
                            const Pattern &aPattern,
                            const StrokeOptions &aStrokeOptions /* = StrokeOptions() */,
                            const DrawOptions &aOptions /* = DrawOptions() */)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);

  cairo_new_path(mContext);
  cairo_rectangle(mContext, aRect.x, aRect.y, aRect.Width(), aRect.Height());

  DrawPattern(aPattern, aStrokeOptions, aOptions, DRAW_STROKE);
}

void
DrawTargetCairo::StrokeLine(const Point &aStart,
                            const Point &aEnd,
                            const Pattern &aPattern,
                            const StrokeOptions &aStrokeOptions /* = StrokeOptions() */,
                            const DrawOptions &aOptions /* = DrawOptions() */)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);

  cairo_new_path(mContext);
  cairo_move_to(mContext, aStart.x, aStart.y);
  cairo_line_to(mContext, aEnd.x, aEnd.y);

  DrawPattern(aPattern, aStrokeOptions, aOptions, DRAW_STROKE);
}

void
DrawTargetCairo::Stroke(const Path *aPath,
                        const Pattern &aPattern,
                        const StrokeOptions &aStrokeOptions /* = StrokeOptions() */,
                        const DrawOptions &aOptions /* = DrawOptions() */)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext, aPath);

  if (aPath->GetBackendType() != BackendType::CAIRO)
    return;

  PathCairo* path = const_cast<PathCairo*>(static_cast<const PathCairo*>(aPath));
  path->SetPathOnContext(mContext);

  DrawPattern(aPattern, aStrokeOptions, aOptions, DRAW_STROKE);
}

void
DrawTargetCairo::Fill(const Path *aPath,
                      const Pattern &aPattern,
                      const DrawOptions &aOptions /* = DrawOptions() */)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext, aPath);

  if (aPath->GetBackendType() != BackendType::CAIRO)
    return;

  PathCairo* path = const_cast<PathCairo*>(static_cast<const PathCairo*>(aPath));
  path->SetPathOnContext(mContext);

  DrawPattern(aPattern, StrokeOptions(), aOptions, DRAW_FILL);
}

bool
DrawTargetCairo::IsCurrentGroupOpaque()
{
  cairo_surface_t* surf = cairo_get_group_target(mContext);

  if (!surf) {
    return false;
  }

  return cairo_surface_get_content(surf) == CAIRO_CONTENT_COLOR;
}

void
DrawTargetCairo::SetFontOptions()
{
  //   This will attempt to detect if the currently set scaled font on the
  // context has enabled subpixel AA. If it is not permitted, then it will
  // downgrade to grayscale AA.
  //   This only currently works effectively for the cairo-ft backend relative
  // to system defaults, as only cairo-ft reflect system defaults in the scaled
  // font state. However, this will work for cairo-ft on both tree Cairo and
  // system Cairo.
  //   Other backends leave the CAIRO_ANTIALIAS_DEFAULT setting untouched while
  // potentially interpreting it as subpixel or even other types of AA that
  // can't be safely equivocated with grayscale AA. For this reason we don't
  // try to also detect and modify the default AA setting, only explicit
  // subpixel AA. These other backends must instead rely on tree Cairo's
  // cairo_surface_set_subpixel_antialiasing extension.

  // If allowing subpixel AA, then leave Cairo's default AA state.
  if (mPermitSubpixelAA) {
    return;
  }

  if (!mFontOptions) {
    mFontOptions = cairo_font_options_create();
    if (!mFontOptions) {
      gfxWarning() << "Failed allocating Cairo font options";
      return;
    }
  }

  // If the current font requests subpixel AA, force it to gray since we don't
  // allow subpixel AA.
  cairo_get_font_options(mContext, mFontOptions);
  cairo_antialias_t antialias = cairo_font_options_get_antialias(mFontOptions);
  if (antialias == CAIRO_ANTIALIAS_SUBPIXEL) {
    cairo_font_options_set_antialias(mFontOptions, CAIRO_ANTIALIAS_GRAY);
    cairo_set_font_options(mContext, mFontOptions);
  }
}

void
DrawTargetCairo::SetPermitSubpixelAA(bool aPermitSubpixelAA)
{
  DrawTarget::SetPermitSubpixelAA(aPermitSubpixelAA);
#ifdef MOZ_TREE_CAIRO
  cairo_surface_set_subpixel_antialiasing(cairo_get_group_target(mContext),
    aPermitSubpixelAA ? CAIRO_SUBPIXEL_ANTIALIASING_ENABLED : CAIRO_SUBPIXEL_ANTIALIASING_DISABLED);
#endif
}

void
DrawTargetCairo::FillGlyphs(ScaledFont *aFont,
                            const GlyphBuffer &aBuffer,
                            const Pattern &aPattern,
                            const DrawOptions &aOptions,
                            const GlyphRenderingOptions*)
{
  if (mTransformSingular) {
    return;
  }

  if (!IsValid()) {
    gfxDebug() << "FillGlyphs bad surface " << cairo_surface_status(cairo_get_group_target(mContext));
    return;
  }

  if (!aFont) {
    gfxDevCrash(LogReason::InvalidFont) << "Invalid scaled font";
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);
  AutoClearDeviceOffset clear(aPattern);

  ScaledFontBase* scaledFont = static_cast<ScaledFontBase*>(aFont);
  cairo_set_scaled_font(mContext, scaledFont->GetCairoScaledFont());

  cairo_pattern_t* pat = GfxPatternToCairoPattern(aPattern, aOptions.mAlpha, GetTransform());
  if (!pat)
    return;

  cairo_set_source(mContext, pat);
  cairo_pattern_destroy(pat);

  cairo_set_antialias(mContext, GfxAntialiasToCairoAntialias(aOptions.mAntialiasMode));

  // Override any font-specific options as necessary.
  SetFontOptions();

  // Convert our GlyphBuffer into a vector of Cairo glyphs. This code can
  // execute millions of times in short periods, so we want to avoid heap
  // allocation whenever possible. So we use an inline vector capacity of 1024
  // bytes (the maximum allowed by mozilla::Vector), which gives an inline
  // length of 1024 / 24 = 42 elements, which is enough to typically avoid heap
  // allocation in ~99% of cases.
  Vector<cairo_glyph_t, 1024 / sizeof(cairo_glyph_t)> glyphs;
  if (!glyphs.resizeUninitialized(aBuffer.mNumGlyphs)) {
    gfxDevCrash(LogReason::GlyphAllocFailedCairo) << "glyphs allocation failed";
    return;
  }
  for (uint32_t i = 0; i < aBuffer.mNumGlyphs; ++i) {
    glyphs[i].index = aBuffer.mGlyphs[i].mIndex;
    glyphs[i].x = aBuffer.mGlyphs[i].mPosition.x;
    glyphs[i].y = aBuffer.mGlyphs[i].mPosition.y;
  }

  cairo_show_glyphs(mContext, &glyphs[0], aBuffer.mNumGlyphs);

  if (cairo_surface_status(cairo_get_group_target(mContext))) {
    gfxDebug() << "Ending FillGlyphs with a bad surface " << cairo_surface_status(cairo_get_group_target(mContext));
  }
}

void
DrawTargetCairo::Mask(const Pattern &aSource,
                      const Pattern &aMask,
                      const DrawOptions &aOptions /* = DrawOptions() */)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);
  AutoClearDeviceOffset clearSource(aSource);
  AutoClearDeviceOffset clearMask(aMask);

  cairo_set_antialias(mContext, GfxAntialiasToCairoAntialias(aOptions.mAntialiasMode));

  cairo_pattern_t* source = GfxPatternToCairoPattern(aSource, aOptions.mAlpha, GetTransform());
  if (!source) {
    return;
  }

  cairo_pattern_t* mask = GfxPatternToCairoPattern(aMask, aOptions.mAlpha, GetTransform());
  if (!mask) {
    cairo_pattern_destroy(source);
    return;
  }

  if (cairo_pattern_status(source) || cairo_pattern_status(mask)) {
    cairo_pattern_destroy(source);
    cairo_pattern_destroy(mask);
    gfxWarning() << "Invalid pattern";
    return;
  }

  cairo_set_source(mContext, source);
  cairo_set_operator(mContext, GfxOpToCairoOp(aOptions.mCompositionOp));
  cairo_mask(mContext, mask);

  cairo_pattern_destroy(mask);
  cairo_pattern_destroy(source);
}

void
DrawTargetCairo::MaskSurface(const Pattern &aSource,
                             SourceSurface *aMask,
                             Point aOffset,
                             const DrawOptions &aOptions)
{
  if (mTransformSingular) {
    return;
  }

  AutoPrepareForDrawing prep(this, mContext);
  AutoClearDeviceOffset clearSource(aSource);
  AutoClearDeviceOffset clearMask(aMask);

  if (!PatternIsCompatible(aSource)) {
    return;
  }

  cairo_set_antialias(mContext, GfxAntialiasToCairoAntialias(aOptions.mAntialiasMode));

  cairo_pattern_t* pat = GfxPatternToCairoPattern(aSource, aOptions.mAlpha, GetTransform());
  if (!pat) {
    return;
  }

  if (cairo_pattern_status(pat)) {
    cairo_pattern_destroy(pat);
    gfxWarning() << "Invalid pattern";
    return;
  }

  cairo_set_source(mContext, pat);

  if (NeedIntermediateSurface(aSource, aOptions)) {
    cairo_push_group_with_content(mContext, CAIRO_CONTENT_COLOR_ALPHA);

    // Don't want operators to be applied twice
    cairo_set_operator(mContext, CAIRO_OPERATOR_OVER);

    // Now draw the content using the desired operator
    cairo_paint_with_alpha(mContext, aOptions.mAlpha);

    cairo_pop_group_to_source(mContext);
  }

  cairo_surface_t* surf = GetCairoSurfaceForSourceSurface(aMask);
  if (!surf) {
    cairo_pattern_destroy(pat);
    return;
  }
  cairo_pattern_t* mask = cairo_pattern_create_for_surface(surf);
  cairo_matrix_t matrix;

  cairo_matrix_init_translate (&matrix, -aOffset.x, -aOffset.y);
  cairo_pattern_set_matrix (mask, &matrix);

  cairo_set_operator(mContext, GfxOpToCairoOp(aOptions.mCompositionOp));

  cairo_mask(mContext, mask);

  cairo_surface_destroy(surf);
  cairo_pattern_destroy(mask);
  cairo_pattern_destroy(pat);
}

void
DrawTargetCairo::PushClip(const Path *aPath)
{
  if (aPath->GetBackendType() != BackendType::CAIRO) {
    return;
  }

  WillChange(aPath);
  cairo_save(mContext);

  PathCairo* path = const_cast<PathCairo*>(static_cast<const PathCairo*>(aPath));

  if (mTransformSingular) {
    cairo_new_path(mContext);
    cairo_rectangle(mContext, 0, 0, 0, 0);
  } else {
    path->SetPathOnContext(mContext);
  }
  cairo_clip_preserve(mContext);
}

void
DrawTargetCairo::PushClipRect(const Rect& aRect)
{
  WillChange();
  cairo_save(mContext);

  cairo_new_path(mContext);
  if (mTransformSingular) {
    cairo_rectangle(mContext, 0, 0, 0, 0);
  } else {
    cairo_rectangle(mContext, aRect.X(), aRect.Y(), aRect.Width(), aRect.Height());
  }
  cairo_clip_preserve(mContext);
}

void
DrawTargetCairo::PopClip()
{
  // save/restore does not affect the path, so no need to call WillChange()

  // cairo_restore will restore the transform too and we don't want to do that
  // so we'll save it now and restore it after the cairo_restore
  cairo_matrix_t mat;
  cairo_get_matrix(mContext, &mat);

  cairo_restore(mContext);

  cairo_set_matrix(mContext, &mat);
}
 
void
DrawTargetCairo::PushLayer(bool aOpaque, Float aOpacity, SourceSurface* aMask,
                          const Matrix& aMaskTransform, const IntRect& aBounds,
                          bool aCopyBackground)
{
  cairo_content_t content = CAIRO_CONTENT_COLOR_ALPHA;

  if (mFormat == SurfaceFormat::A8) {
    content = CAIRO_CONTENT_ALPHA;
  } else if (aOpaque) {
    content = CAIRO_CONTENT_COLOR;
  }

  if (aCopyBackground) {
    cairo_surface_t* source = cairo_get_group_target(mContext);
    cairo_push_group_with_content(mContext, content);
    cairo_surface_t* dest = cairo_get_group_target(mContext);
    cairo_t* ctx = cairo_create(dest);
    cairo_set_source_surface(ctx, source, 0, 0);
    cairo_set_operator(ctx, CAIRO_OPERATOR_SOURCE);
    cairo_paint(ctx);
    cairo_destroy(ctx);
  } else {
    cairo_push_group_with_content(mContext, content);
  }

  PushedLayer layer(aOpacity, mPermitSubpixelAA);

  if (aMask) {
    cairo_surface_t* surf = GetCairoSurfaceForSourceSurface(aMask);
    if (surf) {
      layer.mMaskPattern = cairo_pattern_create_for_surface(surf);
      cairo_matrix_t mat;
      GfxMatrixToCairoMatrix(aMaskTransform, mat);
      cairo_matrix_invert(&mat);
      cairo_pattern_set_matrix(layer.mMaskPattern, &mat);
      cairo_surface_destroy(surf);
    } else {
      gfxCriticalError() << "Failed to get cairo surface for mask surface!";
    }
  }

  mPushedLayers.push_back(layer);

  SetPermitSubpixelAA(aOpaque);
}

void
DrawTargetCairo::PopLayer()
{
  MOZ_ASSERT(mPushedLayers.size());

  cairo_set_operator(mContext, CAIRO_OPERATOR_OVER);

  cairo_pop_group_to_source(mContext);

  PushedLayer layer = mPushedLayers.back();
  mPushedLayers.pop_back();

  if (!layer.mMaskPattern) {
    cairo_paint_with_alpha(mContext, layer.mOpacity);
  } else {
    if (layer.mOpacity != Float(1.0)) {
      cairo_push_group_with_content(mContext, CAIRO_CONTENT_COLOR_ALPHA);

      // Now draw the content using the desired operator
      cairo_paint_with_alpha(mContext, layer.mOpacity);

      cairo_pop_group_to_source(mContext);
    }
    cairo_mask(mContext, layer.mMaskPattern);
  }

  cairo_matrix_t mat;
  GfxMatrixToCairoMatrix(mTransform, mat);
  cairo_set_matrix(mContext, &mat);

  cairo_pattern_destroy(layer.mMaskPattern);
  SetPermitSubpixelAA(layer.mWasPermittingSubpixelAA);
}

already_AddRefed<PathBuilder>
DrawTargetCairo::CreatePathBuilder(FillRule aFillRule /* = FillRule::FILL_WINDING */) const
{
  return MakeAndAddRef<PathBuilderCairo>(aFillRule);
}

void
DrawTargetCairo::ClearSurfaceForUnboundedSource(const CompositionOp &aOperator)
{
  if (aOperator != CompositionOp::OP_SOURCE)
    return;
  cairo_set_operator(mContext, CAIRO_OPERATOR_CLEAR);
  // It doesn't really matter what the source is here, since Paint
  // isn't bounded by the source and the mask covers the entire clip
  // region.
  cairo_paint(mContext);
}


already_AddRefed<GradientStops>
DrawTargetCairo::CreateGradientStops(GradientStop *aStops, uint32_t aNumStops,
                                     ExtendMode aExtendMode) const
{
  return MakeAndAddRef<GradientStopsCairo>(aStops, aNumStops, aExtendMode);
}

already_AddRefed<FilterNode>
DrawTargetCairo::CreateFilter(FilterType aType)
{
  return FilterNodeSoftware::Create(aType);
}

void
DrawTargetCairo::GetGlyphRasterizationMetrics(ScaledFont *aScaledFont, const uint16_t* aGlyphIndices,
                                              uint32_t aNumGlyphs, GlyphMetrics* aGlyphMetrics)
{
  for (uint32_t i = 0; i < aNumGlyphs; i++) {
    cairo_glyph_t glyph;
    cairo_text_extents_t extents;
    glyph.index = aGlyphIndices[i];
    glyph.x = 0;
    glyph.y = 0;
    cairo_glyph_extents(mContext, &glyph, 1, &extents);

    aGlyphMetrics[i].mXBearing = extents.x_bearing;
    aGlyphMetrics[i].mXAdvance = extents.x_advance;
    aGlyphMetrics[i].mYBearing = extents.y_bearing;
    aGlyphMetrics[i].mYAdvance = extents.y_advance;
    aGlyphMetrics[i].mWidth = extents.width;
    aGlyphMetrics[i].mHeight = extents.height;
  }
}

already_AddRefed<SourceSurface>
DrawTargetCairo::CreateSourceSurfaceFromData(unsigned char *aData,
                                             const IntSize &aSize,
                                             int32_t aStride,
                                             SurfaceFormat aFormat) const
{
  if (!aData) {
    gfxWarning() << "DrawTargetCairo::CreateSourceSurfaceFromData null aData";
    return nullptr;
  }

  cairo_surface_t* surf = CopyToImageSurface(aData, IntRect(IntPoint(), aSize),
                                             aStride, aFormat);
  if (!surf) {
    return nullptr;
  }

  RefPtr<SourceSurfaceCairo> source_surf = new SourceSurfaceCairo(surf, aSize, aFormat);
  cairo_surface_destroy(surf);

  return source_surf.forget();
}

#ifdef CAIRO_HAS_XLIB_SURFACE
static cairo_user_data_key_t gDestroyPixmapKey;

struct DestroyPixmapClosure {
  DestroyPixmapClosure(Drawable d, Screen *s) : mPixmap(d), mScreen(s) {}
  ~DestroyPixmapClosure() {
    XFreePixmap(DisplayOfScreen(mScreen), mPixmap);
  }
  Drawable mPixmap;
  Screen *mScreen;
};

static void
DestroyPixmap(void *data)
{
  delete static_cast<DestroyPixmapClosure*>(data);
}
#endif

already_AddRefed<SourceSurface>
DrawTargetCairo::OptimizeSourceSurface(SourceSurface *aSurface) const
{
  RefPtr<SourceSurface> surface(aSurface);
#ifdef CAIRO_HAS_XLIB_SURFACE
  cairo_surface_type_t ctype = cairo_surface_get_type(mSurface);
  if (aSurface->GetType() == SurfaceType::CAIRO &&
      cairo_surface_get_type(
        static_cast<SourceSurfaceCairo*>(aSurface)->GetSurface()) == ctype) {
    return surface.forget();
  }

  if (ctype != CAIRO_SURFACE_TYPE_XLIB) {
    return surface.forget();
  }

  IntSize size = aSurface->GetSize();
  if (!size.width || !size.height) {
    return surface.forget();
  }

  // Although the dimension parameters in the xCreatePixmapReq wire protocol are
  // 16-bit unsigned integers, the server's CreatePixmap returns BadAlloc if
  // either dimension cannot be represented by a 16-bit *signed* integer.
  #define XLIB_IMAGE_SIDE_SIZE_LIMIT 0x7fff

  if (size.width > XLIB_IMAGE_SIDE_SIZE_LIMIT ||
      size.height > XLIB_IMAGE_SIDE_SIZE_LIMIT) {
    return surface.forget();
  }

  SurfaceFormat format = aSurface->GetFormat();
  Screen *screen = cairo_xlib_surface_get_screen(mSurface);
  Display *dpy = DisplayOfScreen(screen);
  XRenderPictFormat* xrenderFormat = nullptr;
  switch (format) {
  case SurfaceFormat::A8R8G8B8_UINT32:
    xrenderFormat = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    break;
  case SurfaceFormat::X8R8G8B8_UINT32:
    xrenderFormat = XRenderFindStandardFormat(dpy, PictStandardRGB24);
    break;
  case SurfaceFormat::A8:
    xrenderFormat = XRenderFindStandardFormat(dpy, PictStandardA8);
    break;
  default:
    return surface.forget();
  }
  if (!xrenderFormat) {
    return surface.forget();
  }

  Drawable pixmap = XCreatePixmap(dpy, RootWindowOfScreen(screen),
                                  size.width, size.height,
                                  xrenderFormat->depth);
  if (!pixmap) {
    return surface.forget();
  }

  auto closure = MakeUnique<DestroyPixmapClosure>(pixmap, screen);

  ScopedCairoSurface csurf(
    cairo_xlib_surface_create_with_xrender_format(dpy, pixmap,
                                                  screen, xrenderFormat,
                                                  size.width, size.height));
  if (!csurf || cairo_surface_status(csurf)) {
    return surface.forget();
  }

  cairo_surface_set_user_data(csurf, &gDestroyPixmapKey,
                              closure.release(), DestroyPixmap);

  RefPtr<DrawTargetCairo> dt = new DrawTargetCairo();
  if (!dt->Init(csurf, size, &format)) {
    return surface.forget();
  }

  dt->CopySurface(aSurface,
                  IntRect(0, 0, size.width, size.height),
                  IntPoint(0, 0));
  dt->Flush();

  surface = new SourceSurfaceCairo(csurf, size, format);
#endif

  return surface.forget();
}

already_AddRefed<SourceSurface>
DrawTargetCairo::CreateSourceSurfaceFromNativeSurface(const NativeSurface &aSurface) const
{
  return nullptr;
}

already_AddRefed<DrawTarget>
DrawTargetCairo::CreateSimilarDrawTarget(const IntSize &aSize, SurfaceFormat aFormat) const
{
  if (cairo_surface_status(cairo_get_group_target(mContext))) {
    RefPtr<DrawTargetCairo> target = new DrawTargetCairo();
    if (target->Init(aSize, aFormat)) {
      return target.forget();
    }
  }

  cairo_surface_t* similar;
  switch (cairo_surface_get_type(mSurface)) {
#ifdef CAIRO_HAS_WIN32_SURFACE
    case CAIRO_SURFACE_TYPE_WIN32:
      similar = cairo_win32_surface_create_with_dib(
        GfxFormatToCairoFormat(aFormat), aSize.width, aSize.height);
      break;
#endif
#ifdef CAIRO_HAS_QUARTZ_SURFACE
    case CAIRO_SURFACE_TYPE_QUARTZ:
      similar = cairo_quartz_surface_create_cg_layer(
        mSurface, GfxFormatToCairoContent(aFormat), aSize.width, aSize.height);
      break;
#endif
    default:
      similar = cairo_surface_create_similar(mSurface,
                                             GfxFormatToCairoContent(aFormat),
                                             aSize.width, aSize.height);
      break;
  }

  if (!cairo_surface_status(similar)) {
    RefPtr<DrawTargetCairo> target = new DrawTargetCairo();
    if (target->InitAlreadyReferenced(similar, aSize)) {
      return target.forget();
    }
  }

  gfxCriticalError(CriticalLog::DefaultOptions(Factory::ReasonableSurfaceSize(aSize))) << "Failed to create similar cairo surface! Size: " << aSize << " Status: " << cairo_surface_status(similar) << cairo_surface_status(cairo_get_group_target(mContext)) << " format " << (int)aFormat;
  cairo_surface_destroy(similar);

  return nullptr;
}

bool
DrawTargetCairo::InitAlreadyReferenced(cairo_surface_t* aSurface, const IntSize& aSize, SurfaceFormat* aFormat)
{
  if (cairo_surface_status(aSurface)) {
    gfxCriticalNote
      << "Attempt to create DrawTarget for invalid surface. "
      << aSize << " Cairo Status: " << cairo_surface_status(aSurface);
    cairo_surface_destroy(aSurface);
    return false;
  }

  mContext = cairo_create(aSurface);
  mSurface = aSurface;
  mSize = aSize;
  mFormat = aFormat ? *aFormat : GfxFormatForCairoSurface(aSurface);

  // Cairo image surface have a bug where they will allocate a mask surface (for clipping)
  // the size of the clip extents, and don't take the surface extents into account.
  // Add a manual clip to the surface extents to prevent this.
  cairo_new_path(mContext);
  cairo_rectangle(mContext, 0, 0, mSize.width, mSize.height);
  cairo_clip(mContext);

  if (mFormat == SurfaceFormat::A8R8G8B8_UINT32 ||
      mFormat == SurfaceFormat::R8G8B8A8) {
    SetPermitSubpixelAA(false);
  } else {
    SetPermitSubpixelAA(true);
  }

  return true;
}

already_AddRefed<DrawTarget>
DrawTargetCairo::CreateShadowDrawTarget(const IntSize &aSize, SurfaceFormat aFormat,
                                        float aSigma) const
{
  cairo_surface_t* similar = cairo_surface_create_similar(cairo_get_target(mContext),
                                                          GfxFormatToCairoContent(aFormat),
                                                          aSize.width, aSize.height);

  if (cairo_surface_status(similar)) {
    return nullptr;
  }

  // If we don't have a blur then we can use the RGBA mask and keep all the
  // operations in graphics memory.
  if (aSigma == 0.0f || aFormat == SurfaceFormat::A8) {
    RefPtr<DrawTargetCairo> target = new DrawTargetCairo();
    if (target->InitAlreadyReferenced(similar, aSize)) {
      return target.forget();
    } else {
      return nullptr;
    }
  }

  cairo_surface_t* blursurf = cairo_image_surface_create(CAIRO_FORMAT_A8,
                                                         aSize.width,
                                                         aSize.height);

  if (cairo_surface_status(blursurf)) {
    return nullptr;
  }

  cairo_surface_t* tee = cairo_tee_surface_create(blursurf);
  cairo_surface_destroy(blursurf);
  if (cairo_surface_status(tee)) {
    cairo_surface_destroy(similar);
    return nullptr;
  }

  cairo_tee_surface_add(tee, similar);
  cairo_surface_destroy(similar);

  RefPtr<DrawTargetCairo> target = new DrawTargetCairo();
  if (target->InitAlreadyReferenced(tee, aSize)) {
    return target.forget();
  }
  return nullptr;
}

static inline pixman_format_code_t
GfxFormatToPixmanFormat(SurfaceFormat aFormat)
{
  switch (aFormat) {
  case SurfaceFormat::A8R8G8B8_UINT32:
    return PIXMAN_a8r8g8b8;
  case SurfaceFormat::X8R8G8B8_UINT32:
    return PIXMAN_x8r8g8b8;
  case SurfaceFormat::R5G6B5_UINT16:
    return PIXMAN_r5g6b5;
  case SurfaceFormat::A8:
    return PIXMAN_a8;
  default:
    // Allow both BGRA and ARGB formats to be passed through unmodified,
    // even though even though we are actually rendering to A8R8G8B8_UINT32.
    if (aFormat == SurfaceFormat::B8G8R8A8 ||
        aFormat == SurfaceFormat::A8R8G8B8) {
      return PIXMAN_a8r8g8b8;
    }
    return (pixman_format_code_t)0;
  }
}

static inline bool
GfxMatrixToPixmanTransform(const Matrix4x4 &aMatrix, pixman_transform* aResult)
{
  pixman_f_transform fTransform = {{
    { aMatrix._11, aMatrix._21, aMatrix._41 },
    { aMatrix._12, aMatrix._22, aMatrix._42 },
    { aMatrix._14, aMatrix._24, aMatrix._44 }
  }};
  return pixman_transform_from_pixman_f_transform(aResult, &fTransform);
}

#ifndef USE_SKIA
bool
DrawTarget::Draw3DTransformedSurface(SourceSurface* aSurface, const Matrix4x4& aMatrix)
{
  // Composite the 3D transform with the DT's transform.
  Matrix4x4 fullMat = aMatrix * Matrix4x4::From2D(mTransform);
  // Transform the surface bounds and clip to this DT.
  IntRect xformBounds =
    RoundedOut(
      fullMat.TransformAndClipBounds(Rect(Point(0, 0), Size(aSurface->GetSize())),
                                     Rect(Point(0, 0), Size(GetSize()))));
  if (xformBounds.IsEmpty()) {
    return true;
  }
  // Offset the matrix by the transformed origin.
  fullMat.PostTranslate(-xformBounds.x, -xformBounds.y, 0);
  // Invert the matrix into a pattern matrix for pixman.
  if (!fullMat.Invert()) {
    return false;
  }
  pixman_transform xform;
  if (!GfxMatrixToPixmanTransform(fullMat, &xform)) {
    return false;
  }

  // Read in the source data.
  RefPtr<DataSourceSurface> srcSurf = aSurface->GetDataSurface();
  pixman_format_code_t srcFormat = GfxFormatToPixmanFormat(srcSurf->GetFormat());
  if (!srcFormat) {
    return false;
  }
  DataSourceSurface::ScopedMap srcMap(srcSurf, DataSourceSurface::READ);
  if (!srcMap.IsMapped()) {
    return false;
  }

  // Set up an intermediate destination surface only the size of the transformed bounds.
  // Try to pass through the source's format unmodified in both the BGRA and ARGB cases.
  RefPtr<DataSourceSurface> dstSurf =
    Factory::CreateDataSourceSurface(xformBounds.Size(),
                                     srcFormat == PIXMAN_a8r8g8b8 ?
                                       srcSurf->GetFormat() : SurfaceFormat::A8R8G8B8_UINT32);
  if (!dstSurf) {
    return false;
  }

  // Wrap the surfaces in pixman images and do the transform.
  pixman_image_t* dst =
    pixman_image_create_bits(PIXMAN_a8r8g8b8,
                             xformBounds.width, xformBounds.height,
                             (uint32_t*)dstSurf->GetData(), dstSurf->Stride());
  if (!dst) {
    return false;
  }
  pixman_image_t* src =
    pixman_image_create_bits(srcFormat,
                             srcSurf->GetSize().width, srcSurf->GetSize().height,
                             (uint32_t*)srcMap.GetData(), srcMap.GetStride());
  if (!src) {
    pixman_image_unref(dst);
    return false;
  }

  pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, nullptr, 0);
  pixman_image_set_transform(src, &xform);

  pixman_image_composite32(PIXMAN_OP_SRC,
                           src, nullptr, dst,
                           0, 0, 0, 0, 0, 0,
                           xformBounds.width, xformBounds.height);

  pixman_image_unref(dst);
  pixman_image_unref(src);

  // Temporarily reset the DT's transform, since it has already been composed above.
  Matrix origTransform = mTransform;
  SetTransform(Matrix());

  // Draw the transformed surface within the transformed bounds.
  DrawSurface(dstSurf, Rect(xformBounds), Rect(Point(0, 0), Size(xformBounds.Size())));

  SetTransform(origTransform);

  return true;
}
#endif

#ifdef CAIRO_HAS_XLIB_SURFACE
static bool gXRenderInitialized = false;
static bool gXRenderHasTransform = false;

static bool
SupportsXRender(cairo_surface_t* surface)
{
  if (!surface ||
      cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_XLIB ||
      !cairo_xlib_surface_get_xrender_format(surface)) {
    return false;
  }

  if (gXRenderInitialized) {
    return true;
  }
  gXRenderInitialized = true;

  cairo_device_t* device = cairo_surface_get_device(surface);
  if (cairo_device_acquire(device) != CAIRO_STATUS_SUCCESS) {
    return false;
  }

  Display* display = cairo_xlib_surface_get_display(surface);
  int major, minor;
  if (XRenderQueryVersion(display, &major, &minor)) {
    if (major > 0 || (major == 0 && minor >= 6)) {
      gXRenderHasTransform = true;
    }
  }

  cairo_device_release(device);

  return true;
}
#endif

bool
DrawTargetCairo::Draw3DTransformedSurface(SourceSurface* aSurface, const Matrix4x4& aMatrix)
{
#if CAIRO_HAS_XLIB_SURFACE
  cairo_surface_t* srcSurf =
    aSurface->GetType() == SurfaceType::CAIRO ?
      static_cast<SourceSurfaceCairo*>(aSurface)->GetSurface() : nullptr;
  if (!SupportsXRender(srcSurf) || !gXRenderHasTransform) {
    return DrawTarget::Draw3DTransformedSurface(aSurface, aMatrix);
  }

  Matrix4x4 fullMat = aMatrix * Matrix4x4::From2D(mTransform);
  IntRect xformBounds =
    RoundedOut(
      fullMat.TransformAndClipBounds(Rect(Point(0, 0), Size(aSurface->GetSize())),
                                     Rect(Point(0, 0), Size(GetSize()))));
  if (xformBounds.IsEmpty()) {
    return true;
  }
  fullMat.PostTranslate(-xformBounds.x, -xformBounds.y, 0);
  if (!fullMat.Invert()) {
    return false;
  }
  pixman_transform xform;
  if (!GfxMatrixToPixmanTransform(fullMat, &xform)) {
    return false;
  }

  cairo_surface_t* xformSurf =
    cairo_surface_create_similar(srcSurf, CAIRO_CONTENT_COLOR_ALPHA,
                                 xformBounds.width, xformBounds.height);
  if (!SupportsXRender(xformSurf)) {
    cairo_surface_destroy(xformSurf);
    return false;
  }
  cairo_device_t* device = cairo_surface_get_device(xformSurf);
  if (cairo_device_acquire(device) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(xformSurf);
    return false;
  }

  Display* display = cairo_xlib_surface_get_display(xformSurf);

  Picture srcPict = XRenderCreatePicture(display,
                                         cairo_xlib_surface_get_drawable(srcSurf),
                                         cairo_xlib_surface_get_xrender_format(srcSurf),
                                         0, nullptr);
  XRenderSetPictureFilter(display, srcPict, FilterBilinear, nullptr, 0);
  XRenderSetPictureTransform(display, srcPict, (XTransform*)&xform);

  Picture dstPict = XRenderCreatePicture(display,
                                         cairo_xlib_surface_get_drawable(xformSurf),
                                         cairo_xlib_surface_get_xrender_format(xformSurf),
                                         0, nullptr);

  XRenderComposite(display, PictOpSrc,
                   srcPict, X11None, dstPict,
                   0, 0, 0, 0, 0, 0,
                   xformBounds.width, xformBounds.height);

  XRenderFreePicture(display, srcPict);
  XRenderFreePicture(display, dstPict);

  cairo_device_release(device);
  cairo_surface_mark_dirty(xformSurf);

  AutoPrepareForDrawing(this, mContext);

  cairo_identity_matrix(mContext);

  cairo_set_operator(mContext, CAIRO_OPERATOR_OVER);
  cairo_set_antialias(mContext, CAIRO_ANTIALIAS_DEFAULT);
  cairo_set_source_surface(mContext, xformSurf, xformBounds.x, xformBounds.y);

  cairo_new_path(mContext);
  cairo_rectangle(mContext, xformBounds.x, xformBounds.y, xformBounds.width, xformBounds.height);
  cairo_fill(mContext);

  cairo_surface_destroy(xformSurf);

  return true;
#else
  return DrawTarget::Draw3DTransformedSurface(aSurface, aMatrix);
#endif
}

bool
DrawTargetCairo::Init(cairo_surface_t* aSurface, const IntSize& aSize, SurfaceFormat* aFormat)
{
  cairo_surface_reference(aSurface);
  return InitAlreadyReferenced(aSurface, aSize, aFormat);
}

bool
DrawTargetCairo::Init(const IntSize& aSize, SurfaceFormat aFormat)
{
  cairo_surface_t *surf = cairo_image_surface_create(GfxFormatToCairoFormat(aFormat), aSize.width, aSize.height);
  return InitAlreadyReferenced(surf, aSize);
}

bool
DrawTargetCairo::Init(unsigned char* aData, const IntSize &aSize, int32_t aStride, SurfaceFormat aFormat)
{
  cairo_surface_t* surf =
    cairo_image_surface_create_for_data(aData,
                                        GfxFormatToCairoFormat(aFormat),
                                        aSize.width,
                                        aSize.height,
                                        aStride);
  return InitAlreadyReferenced(surf, aSize);
}

void *
DrawTargetCairo::GetNativeSurface(NativeSurfaceType aType)
{
  if (aType == NativeSurfaceType::CAIRO_CONTEXT) {
    return mContext;
  }

  return nullptr;
}

void
DrawTargetCairo::MarkSnapshotIndependent()
{
  if (mSnapshot) {
    if (mSnapshot->refCount() > 1) {
      // We only need to worry about snapshots that someone else knows about
      mSnapshot->DrawTargetWillChange();
    }
    mSnapshot = nullptr;
  }
}

void
DrawTargetCairo::WillChange(const Path* aPath /* = nullptr */)
{
  MarkSnapshotIndependent();
  MOZ_ASSERT(!mLockedBits);
}

void
DrawTargetCairo::SetTransform(const Matrix& aTransform)
{
  DrawTarget::SetTransform(aTransform);

  mTransformSingular = aTransform.IsSingular();
  if (!mTransformSingular) {
    cairo_matrix_t mat;
    GfxMatrixToCairoMatrix(mTransform, mat);
    cairo_set_matrix(mContext, &mat);
  }
}

Rect
DrawTargetCairo::GetUserSpaceClip()
{
  double clipX1, clipY1, clipX2, clipY2;
  cairo_clip_extents(mContext, &clipX1, &clipY1, &clipX2, &clipY2);
  return Rect(clipX1, clipY1, clipX2 - clipX1, clipY2 - clipY1); // Narrowing of doubles to floats
}

cairo_t*
BorrowedCairoContext::BorrowCairoContextFromDrawTarget(DrawTarget* aDT)
{
  if (aDT->GetBackendType() != BackendType::CAIRO ||
      aDT->IsDualDrawTarget() ||
      aDT->IsTiledDrawTarget()) {
    return nullptr;
  }
  DrawTargetCairo* cairoDT = static_cast<DrawTargetCairo*>(aDT);

  cairoDT->WillChange();

  // save the state to make it easier for callers to avoid mucking with things
  cairo_save(cairoDT->mContext);

  // Neuter the DrawTarget while the context is being borrowed
  cairo_t* cairo = cairoDT->mContext;
  cairoDT->mContext = nullptr;

  return cairo;
}

void
BorrowedCairoContext::ReturnCairoContextToDrawTarget(DrawTarget* aDT,
                                                     cairo_t* aCairo)
{
  if (aDT->GetBackendType() != BackendType::CAIRO ||
      aDT->IsDualDrawTarget() ||
      aDT->IsTiledDrawTarget()) {
    return;
  }
  DrawTargetCairo* cairoDT = static_cast<DrawTargetCairo*>(aDT);

  cairo_restore(aCairo);
  cairoDT->mContext = aCairo;
}

#ifdef MOZ_X11
bool
BorrowedXlibDrawable::Init(DrawTarget* aDT)
{
  MOZ_ASSERT(aDT, "Caller should check for nullptr");
  MOZ_ASSERT(!mDT, "Can't initialize twice!");
  mDT = aDT;
  mDrawable = X11None;

#ifdef CAIRO_HAS_XLIB_SURFACE
  if (aDT->GetBackendType() != BackendType::CAIRO ||
      aDT->IsDualDrawTarget() ||
      aDT->IsTiledDrawTarget()) {
    return false;
  }

  DrawTargetCairo* cairoDT = static_cast<DrawTargetCairo*>(aDT);
  cairo_surface_t* surf = cairo_get_group_target(cairoDT->mContext);
  if (cairo_surface_get_type(surf) != CAIRO_SURFACE_TYPE_XLIB) {
    return false;
  }
  cairo_surface_flush(surf);

  cairoDT->WillChange();

  mDisplay = cairo_xlib_surface_get_display(surf);
  mDrawable = cairo_xlib_surface_get_drawable(surf);
  mScreen = cairo_xlib_surface_get_screen(surf);
  mVisual = cairo_xlib_surface_get_visual(surf);
  mXRenderFormat = cairo_xlib_surface_get_xrender_format(surf);
  mSize.width = cairo_xlib_surface_get_width(surf);
  mSize.height = cairo_xlib_surface_get_height(surf);

  double x = 0, y = 0;
  cairo_surface_get_device_offset(surf, &x, &y);
  mOffset = Point(x, y);

  return true;
#else
  return false;
#endif
}

void
BorrowedXlibDrawable::Finish()
{
  DrawTargetCairo* cairoDT = static_cast<DrawTargetCairo*>(mDT);
  cairo_surface_t* surf = cairo_get_group_target(cairoDT->mContext);
  cairo_surface_mark_dirty(surf);
  if (mDrawable) {
    mDrawable = X11None;
  }
}
#endif

} // namespace gfx
} // namespace mozilla
