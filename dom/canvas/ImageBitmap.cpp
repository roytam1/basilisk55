/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ImageBitmap.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/dom/ImageBitmapBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/StructuredCloneTags.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerRunnable.h"
#include "mozilla/gfx/2D.h"
#include "ImageBitmapColorUtils.h"
#include "ImageBitmapUtils.h"
#include "ImageUtils.h"
#include "imgTools.h"
#include "libyuv.h"

using namespace mozilla::gfx;
using namespace mozilla::layers;

namespace mozilla {
namespace dom {

using namespace workers;

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(ImageBitmap, mParent)
NS_IMPL_CYCLE_COLLECTING_ADDREF(ImageBitmap)
NS_IMPL_CYCLE_COLLECTING_RELEASE(ImageBitmap)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ImageBitmap)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

/*
 * This helper function is used to notify DOM that aBytes memory is allocated
 * here so that we could trigger GC appropriately.
 */
static void
RegisterAllocation(nsIGlobalObject* aGlobal, size_t aBytes)
{
  AutoJSAPI jsapi;
  if (jsapi.Init(aGlobal)) {
    JS_updateMallocCounter(jsapi.cx(), aBytes);
  }
}

static void
RegisterAllocation(nsIGlobalObject* aGlobal, SourceSurface* aSurface)
{
  // Calculate how many bytes are used.
  const int bytesPerPixel = BytesPerPixel(aSurface->GetFormat());
  const size_t bytes =
    aSurface->GetSize().height * aSurface->GetSize().width * bytesPerPixel;

  // Register.
  RegisterAllocation(aGlobal, bytes);
}

static void
RegisterAllocation(nsIGlobalObject* aGlobal, layers::Image* aImage)
{
  // Calculate how many bytes are used.
  if (aImage->GetFormat() == mozilla::ImageFormat::PLANAR_YCBCR) {
    RegisterAllocation(aGlobal, aImage->AsPlanarYCbCrImage()->GetDataSize());
  } else if (aImage->GetFormat() == mozilla::ImageFormat::NV_IMAGE) {
    RegisterAllocation(aGlobal, aImage->AsNVImage()->GetBufferSize());
  } else {
    RefPtr<SourceSurface> surface = aImage->GetAsSourceSurface();
    RegisterAllocation(aGlobal, surface);
  }
}

/*
 * If either aRect.width or aRect.height are negative, then return a new IntRect
 * which represents the same rectangle as the aRect does but with positive width
 * and height.
 */
static IntRect
FixUpNegativeDimension(const IntRect& aRect, ErrorResult& aRv)
{
  gfx::IntRect rect = aRect;

  // fix up negative dimensions
  if (rect.width < 0) {
    CheckedInt32 checkedX = CheckedInt32(rect.x) + rect.width;

    if (!checkedX.isValid()) {
      aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
      return rect;
    }

    rect.x = checkedX.value();
    rect.width = -(rect.width);
  }

  if (rect.height < 0) {
    CheckedInt32 checkedY = CheckedInt32(rect.y) + rect.height;

    if (!checkedY.isValid()) {
      aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
      return rect;
    }

    rect.y = checkedY.value();
    rect.height = -(rect.height);
  }

  return rect;
}

/*
 * This helper function copies the data of the given DataSourceSurface,
 *  _aSurface_, in the given area, _aCropRect_, into a new DataSourceSurface.
 * This might return null if it can not create a new SourceSurface or it cannot
 * read data from the given _aSurface_.
 *
 * Warning: Even though the area of _aCropRect_ is just the same as the size of
 *          _aSurface_, this function still copy data into a new
 *          DataSourceSurface.
 */
static already_AddRefed<DataSourceSurface>
CropAndCopyDataSourceSurface(DataSourceSurface* aSurface, const IntRect& aCropRect)
{
  MOZ_ASSERT(aSurface);

  // Check the aCropRect
  ErrorResult error;
  const IntRect positiveCropRect = FixUpNegativeDimension(aCropRect, error);
  if (NS_WARN_IF(error.Failed())) {
    error.SuppressException();
    return nullptr;
  }

  // Calculate the size of the new SourceSurface.
  // We cannot keep using aSurface->GetFormat() to create new DataSourceSurface,
  // since it might be SurfaceFormat::B8G8R8X8 which does not handle opacity,
  // however the specification explicitly define that "If any of the pixels on
  // this rectangle are outside the area where the input bitmap was placed, then
  // they will be transparent black in output."
  // So, instead, we force the output format to be SurfaceFormat::B8G8R8A8.
  const SurfaceFormat format = SurfaceFormat::B8G8R8A8;
  const int bytesPerPixel = BytesPerPixel(format);
  const IntSize dstSize = IntSize(positiveCropRect.width,
                                  positiveCropRect.height);
  const uint32_t dstStride = dstSize.width * bytesPerPixel;

  // Create a new SourceSurface.
  RefPtr<DataSourceSurface> dstDataSurface =
    Factory::CreateDataSourceSurfaceWithStride(dstSize, format, dstStride, true);

  if (NS_WARN_IF(!dstDataSurface)) {
    return nullptr;
  }

  // Only do copying and cropping when the positiveCropRect intersects with
  // the size of aSurface.
  const IntRect surfRect(IntPoint(0, 0), aSurface->GetSize());
  if (surfRect.Intersects(positiveCropRect)) {
    const IntRect surfPortion = surfRect.Intersect(positiveCropRect);
    const IntPoint dest(std::max(0, surfPortion.X() - positiveCropRect.X()),
                        std::max(0, surfPortion.Y() - positiveCropRect.Y()));

    // Copy the raw data into the newly created DataSourceSurface.
    DataSourceSurface::ScopedMap srcMap(aSurface, DataSourceSurface::READ);
    DataSourceSurface::ScopedMap dstMap(dstDataSurface, DataSourceSurface::WRITE);
    if (NS_WARN_IF(!srcMap.IsMapped()) ||
        NS_WARN_IF(!dstMap.IsMapped())) {
      return nullptr;
    }

    uint8_t* srcBufferPtr = srcMap.GetData() + surfPortion.y * srcMap.GetStride()
                                             + surfPortion.x * bytesPerPixel;
    uint8_t* dstBufferPtr = dstMap.GetData() + dest.y * dstMap.GetStride()
                                             + dest.x * bytesPerPixel;
    CheckedInt<uint32_t> copiedBytesPerRaw =
      CheckedInt<uint32_t>(surfPortion.width) * bytesPerPixel;
    if (!copiedBytesPerRaw.isValid()) {
      return nullptr;
    }

    for (int i = 0; i < surfPortion.height; ++i) {
      memcpy(dstBufferPtr, srcBufferPtr, copiedBytesPerRaw.value());
      srcBufferPtr += srcMap.GetStride();
      dstBufferPtr += dstMap.GetStride();
    }
  }

  return dstDataSurface.forget();
}

/*
 * Encapsulate the given _aSurface_ into a layers::SourceSurfaceImage.
 */
static already_AddRefed<layers::Image>
CreateImageFromSurface(SourceSurface* aSurface)
{
  MOZ_ASSERT(aSurface);
  RefPtr<layers::SourceSurfaceImage> image =
    new layers::SourceSurfaceImage(aSurface->GetSize(), aSurface);
  return image.forget();
}

/*
 * CreateImageFromRawData(), CreateSurfaceFromRawData() and
 * CreateImageFromRawDataInMainThreadSyncTask are helpers for
 * create-from-ImageData case
 */
static already_AddRefed<SourceSurface>
CreateSurfaceFromRawData(const gfx::IntSize& aSize,
                         uint32_t aStride,
                         gfx::SurfaceFormat aFormat,
                         uint8_t* aBuffer,
                         uint32_t aBufferLength,
                         const Maybe<IntRect>& aCropRect)
{
  MOZ_ASSERT(!aSize.IsEmpty());
  MOZ_ASSERT(aBuffer);

  // Wrap the source buffer into a SourceSurface.
  RefPtr<DataSourceSurface> dataSurface =
    Factory::CreateWrappingDataSourceSurface(aBuffer, aStride, aSize, aFormat);

  if (NS_WARN_IF(!dataSurface)) {
    return nullptr;
  }

  // The temporary cropRect variable is equal to the size of source buffer if we
  // do not need to crop, or it equals to the given cropping size.
  const IntRect cropRect = aCropRect.valueOr(IntRect(0, 0, aSize.width, aSize.height));

  // Copy the source buffer in the _cropRect_ area into a new SourceSurface.
  RefPtr<DataSourceSurface> result = CropAndCopyDataSourceSurface(dataSurface, cropRect);

  if (NS_WARN_IF(!result)) {
    return nullptr;
  }

  return result.forget();
}

static already_AddRefed<layers::Image>
CreateImageFromRawData(const gfx::IntSize& aSize,
                       uint32_t aStride,
                       gfx::SurfaceFormat aFormat,
                       uint8_t* aBuffer,
                       uint32_t aBufferLength,
                       const Maybe<IntRect>& aCropRect)
{
  MOZ_ASSERT(NS_IsMainThread());

  // Copy and crop the source buffer into a SourceSurface.
  RefPtr<SourceSurface> rgbaSurface =
    CreateSurfaceFromRawData(aSize, aStride, aFormat,
                             aBuffer, aBufferLength,
                             aCropRect);

  if (NS_WARN_IF(!rgbaSurface)) {
    return nullptr;
  }

  // Convert RGBA to BGRA
  RefPtr<DataSourceSurface> rgbaDataSurface = rgbaSurface->GetDataSurface();
  RefPtr<DataSourceSurface> bgraDataSurface =
    Factory::CreateDataSourceSurfaceWithStride(rgbaDataSurface->GetSize(),
                                               SurfaceFormat::B8G8R8A8,
                                               rgbaDataSurface->Stride());

  DataSourceSurface::MappedSurface rgbaMap;
  DataSourceSurface::MappedSurface bgraMap;

  if (NS_WARN_IF(!rgbaDataSurface->Map(DataSourceSurface::MapType::READ, &rgbaMap)) ||
      NS_WARN_IF(!bgraDataSurface->Map(DataSourceSurface::MapType::WRITE, &bgraMap))) {
    return nullptr;
  }

  libyuv::ABGRToARGB(rgbaMap.mData, rgbaMap.mStride,
                     bgraMap.mData, bgraMap.mStride,
                     bgraDataSurface->GetSize().width,
                     bgraDataSurface->GetSize().height);

  rgbaDataSurface->Unmap();
  bgraDataSurface->Unmap();

  // Create an Image from the BGRA SourceSurface.
  RefPtr<layers::Image> image = CreateImageFromSurface(bgraDataSurface);

  if (NS_WARN_IF(!image)) {
    return nullptr;
  }

  return image.forget();
}

/*
 * This is a synchronous task.
 * This class is used to create a layers::SourceSurfaceImage from raw data in the main
 * thread. While creating an ImageBitmap from an ImageData, we need to create
 * a SouceSurface from the ImageData's raw data and then set the SourceSurface
 * into a layers::SourceSurfaceImage. However, the layers::SourceSurfaceImage asserts the
 * setting operation in the main thread, so if we are going to create an
 * ImageBitmap from an ImageData off the main thread, we post an event to the
 * main thread to create a layers::SourceSurfaceImage from an ImageData's raw data.
 */
class CreateImageFromRawDataInMainThreadSyncTask final :
  public WorkerMainThreadRunnable
{
public:
  CreateImageFromRawDataInMainThreadSyncTask(uint8_t* aBuffer,
                                             uint32_t aBufferLength,
                                             uint32_t aStride,
                                             gfx::SurfaceFormat aFormat,
                                             const gfx::IntSize& aSize,
                                             const Maybe<IntRect>& aCropRect,
                                             layers::Image** aImage)
  : WorkerMainThreadRunnable(GetCurrentThreadWorkerPrivate(),
                               NS_LITERAL_CSTRING("ImageBitmap :: Create Image from Raw Data"))
  , mImage(aImage)
  , mBuffer(aBuffer)
  , mBufferLength(aBufferLength)
  , mStride(aStride)
  , mFormat(aFormat)
  , mSize(aSize)
  , mCropRect(aCropRect)
  {
    MOZ_ASSERT(!(*aImage), "Don't pass an existing Image into CreateImageFromRawDataInMainThreadSyncTask.");
  }

  bool MainThreadRun() override
  {
    RefPtr<layers::Image> image =
      CreateImageFromRawData(mSize, mStride, mFormat,
                             mBuffer, mBufferLength,
                             mCropRect);

    if (NS_WARN_IF(!image)) {
      return false;
    }

    image.forget(mImage);

    return true;
  }

private:
  layers::Image** mImage;
  uint8_t* mBuffer;
  uint32_t mBufferLength;
  uint32_t mStride;
  gfx::SurfaceFormat mFormat;
  gfx::IntSize mSize;
  const Maybe<IntRect>& mCropRect;
};

static bool
CheckSecurityForHTMLElements(bool aIsWriteOnly, bool aCORSUsed, nsIPrincipal* aPrincipal)
{
  MOZ_ASSERT(aPrincipal);

  if (aIsWriteOnly) {
    return false;
  }

  if (!aCORSUsed) {
    nsIGlobalObject* incumbentSettingsObject = GetIncumbentGlobal();
    if (NS_WARN_IF(!incumbentSettingsObject)) {
      return false;
    }

    nsIPrincipal* principal = incumbentSettingsObject->PrincipalOrNull();
    if (NS_WARN_IF(!principal) || !(principal->Subsumes(aPrincipal))) {
      return false;
    }
  }

  return true;
}

static bool
CheckSecurityForHTMLElements(const nsLayoutUtils::SurfaceFromElementResult& aRes)
{
  return CheckSecurityForHTMLElements(aRes.mIsWriteOnly, aRes.mCORSUsed, aRes.mPrincipal);
}

/*
 * A wrapper to the nsLayoutUtils::SurfaceFromElement() function followed by the
 * security checking.
 */
template<class HTMLElementType>
static already_AddRefed<SourceSurface>
GetSurfaceFromElement(nsIGlobalObject* aGlobal, HTMLElementType& aElement,
                      bool* aWriteOnly, ErrorResult& aRv)
{
  nsLayoutUtils::SurfaceFromElementResult res =
    nsLayoutUtils::SurfaceFromElement(&aElement, nsLayoutUtils::SFE_WANT_FIRST_FRAME);

  RefPtr<SourceSurface> surface = res.GetSourceSurface();

  if (NS_WARN_IF(!surface)) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }
  
  // Check origin-clean and pass back
  *aWriteOnly = !CheckSecurityForHTMLElements(res);

  return surface.forget();
}

/*
 * The specification doesn't allow to create an ImageBitmap from a vector image.
 * This function is used to check if the given HTMLImageElement contains a
 * raster image.
 */
static bool
HasRasterImage(HTMLImageElement& aImageEl)
{
  nsresult rv;

  nsCOMPtr<imgIRequest> imgRequest;
  rv = aImageEl.GetRequest(nsIImageLoadingContent::CURRENT_REQUEST,
                           getter_AddRefs(imgRequest));
  if (NS_SUCCEEDED(rv) && imgRequest) {
    nsCOMPtr<imgIContainer> imgContainer;
    rv = imgRequest->GetImage(getter_AddRefs(imgContainer));
    if (NS_SUCCEEDED(rv) && imgContainer &&
        imgContainer->GetType() == imgIContainer::TYPE_RASTER) {
      return true;
    }
  }

  return false;
}

ImageBitmap::ImageBitmap(nsIGlobalObject* aGlobal, layers::Image* aData,
                         bool aWriteOnly, bool aIsPremultipliedAlpha /* = true */)
  : mParent(aGlobal)
  , mData(aData)
  , mSurface(nullptr)
  , mDataWrapper(new ImageUtils(mData))
  , mPictureRect(0, 0, aData->GetSize().width, aData->GetSize().height)
  , mIsPremultipliedAlpha(aIsPremultipliedAlpha)
  , mWriteOnly(aWriteOnly)
{
  MOZ_ASSERT(aData, "aData is null in ImageBitmap constructor.");
}

ImageBitmap::~ImageBitmap()
{
}

JSObject*
ImageBitmap::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return ImageBitmapBinding::Wrap(aCx, this, aGivenProto);
}

void
ImageBitmap::Close()
{
  mData = nullptr;
  mSurface = nullptr;
  mPictureRect.SetEmpty();
}

void
ImageBitmap::SetPictureRect(const IntRect& aRect, ErrorResult& aRv)
{
  mPictureRect = FixUpNegativeDimension(aRect, aRv);
}

static already_AddRefed<SourceSurface>
ConvertColorFormatIfNeeded(RefPtr<SourceSurface> aSurface)
{
  const SurfaceFormat srcFormat = aSurface->GetFormat();
  if (srcFormat == SurfaceFormat::R8G8B8A8 ||
      srcFormat == SurfaceFormat::B8G8R8A8 ||
      srcFormat == SurfaceFormat::R8G8B8X8 ||
      srcFormat == SurfaceFormat::B8G8R8X8 ||
      srcFormat == SurfaceFormat::A8R8G8B8 ||
      srcFormat == SurfaceFormat::X8R8G8B8) {
    return aSurface.forget();
  }

  if (srcFormat == SurfaceFormat::A8 ||
      srcFormat == SurfaceFormat::Depth) {
    return nullptr;
  }

  const int bytesPerPixel = BytesPerPixel(SurfaceFormat::B8G8R8A8);
  const IntSize dstSize = aSurface->GetSize();
  const uint32_t dstStride = dstSize.width * bytesPerPixel;

  RefPtr<DataSourceSurface> dstDataSurface =
    Factory::CreateDataSourceSurfaceWithStride(dstSize,
                                               SurfaceFormat::B8G8R8A8,
                                               dstStride);

  RefPtr<DataSourceSurface> srcDataSurface = aSurface->GetDataSurface();
  if (NS_WARN_IF(!srcDataSurface)) {
    return nullptr;
  }

  DataSourceSurface::ScopedMap srcMap(srcDataSurface, DataSourceSurface::READ);
  DataSourceSurface::ScopedMap dstMap(dstDataSurface, DataSourceSurface::WRITE);
  if (NS_WARN_IF(!srcMap.IsMapped()) || NS_WARN_IF(!dstMap.IsMapped())) {
    return nullptr;
  }

  int rv = 0;
  if (srcFormat == SurfaceFormat::R8G8B8) {
    rv = RGB24ToBGRA32(srcMap.GetData(), srcMap.GetStride(),
                       dstMap.GetData(), dstMap.GetStride(),
                       dstSize.width, dstSize.height);
  } else if (srcFormat == SurfaceFormat::B8G8R8) {
    rv = BGR24ToBGRA32(srcMap.GetData(), srcMap.GetStride(),
                       dstMap.GetData(), dstMap.GetStride(),
                       dstSize.width, dstSize.height);
  } else if (srcFormat == SurfaceFormat::HSV) {
    rv = HSVToBGRA32((const float*)srcMap.GetData(), srcMap.GetStride(),
                     dstMap.GetData(), dstMap.GetStride(),
                     dstSize.width, dstSize.height);
  } else if (srcFormat == SurfaceFormat::Lab) {
    rv = LabToBGRA32((const float*)srcMap.GetData(), srcMap.GetStride(),
                     dstMap.GetData(), dstMap.GetStride(),
                     dstSize.width, dstSize.height);
  }

  if (NS_WARN_IF(rv != 0)) {
    return nullptr;
  }

  return dstDataSurface.forget();
}

/*
 * The functionality of PrepareForDrawTarget method:
 * (1) Get a SourceSurface from the mData (which is a layers::Image).
 * (2) Convert the SourceSurface to format B8G8R8A8 if the original format is
 *     R8G8B8, B8G8R8, HSV or Lab.
 *     Note: if the original format is A8 or Depth, then return null directly.
 * (3) Do cropping if the size of SourceSurface does not equal to the
 *     mPictureRect.
 * (4) Pre-multiply alpha if needed.
 */
already_AddRefed<SourceSurface>
ImageBitmap::PrepareForDrawTarget(gfx::DrawTarget* aTarget)
{
  MOZ_ASSERT(aTarget);

  if (!mData) {
    return nullptr;
  }

  if (!mSurface) {
    mSurface = mData->GetAsSourceSurface();

    if (!mSurface) {
      return nullptr;
    }
  }

  // Check if we need to convert the format.
  // Convert R8G8B8/B8G8R8/HSV/Lab to B8G8R8A8.
  // Return null if the original format is A8 or Depth.
  mSurface = ConvertColorFormatIfNeeded(mSurface);
  if (NS_WARN_IF(!mSurface)) {
    return nullptr;
  }

  RefPtr<DrawTarget> target = aTarget;
  IntRect surfRect(0, 0, mSurface->GetSize().width, mSurface->GetSize().height);

  // Check if we still need to crop our surface
  if (!mPictureRect.IsEqualEdges(surfRect)) {

    IntRect surfPortion = surfRect.Intersect(mPictureRect);

    // the crop lies entirely outside the surface area, nothing to draw
    if (surfPortion.IsEmpty()) {
      mSurface = nullptr;
      RefPtr<gfx::SourceSurface> surface(mSurface);
      return surface.forget();
    }

    IntPoint dest(std::max(0, surfPortion.X() - mPictureRect.X()),
                  std::max(0, surfPortion.Y() - mPictureRect.Y()));

    // We must initialize this target with mPictureRect.Size() because the
    // specification states that if the cropping area is given, then return an
    // ImageBitmap with the size equals to the cropping area.
    target = target->CreateSimilarDrawTarget(mPictureRect.Size(),
                                             target->GetFormat());

    if (!target) {
      mSurface = nullptr;
      RefPtr<gfx::SourceSurface> surface(mSurface);
      return surface.forget();
    }

    target->CopySurface(mSurface, surfPortion, dest);
    mSurface = target->Snapshot();

    // Make mCropRect match new surface we've cropped to
    mPictureRect.MoveTo(0, 0);
  }

  // Pre-multiply alpha here.
  // Apply pre-multiply alpha only if mIsPremultipliedAlpha is false.
  // Ignore this step if the source surface does not have alpha channel; this
  // kind of source surfaces might come form layers::PlanarYCbCrImage.
  if (!mIsPremultipliedAlpha &&
      mSurface->GetFormat() != SurfaceFormat::B8G8R8X8 &&
      mSurface->GetFormat() != SurfaceFormat::R8G8B8X8 &&
      mSurface->GetFormat() != SurfaceFormat::X8R8G8B8) {
    MOZ_ASSERT(mSurface->GetFormat() == SurfaceFormat::R8G8B8A8 ||
               mSurface->GetFormat() == SurfaceFormat::B8G8R8A8 ||
               mSurface->GetFormat() == SurfaceFormat::A8R8G8B8);

    RefPtr<DataSourceSurface> dstSurface = mSurface->GetDataSurface();
    MOZ_ASSERT(dstSurface);

    RefPtr<DataSourceSurface> srcSurface;
    DataSourceSurface::MappedSurface srcMap;
    DataSourceSurface::MappedSurface dstMap;

    if (!dstSurface->Map(DataSourceSurface::MapType::READ_WRITE, &dstMap)) {
      srcSurface = dstSurface;
      if (!srcSurface->Map(DataSourceSurface::READ, &srcMap)) {
        gfxCriticalError() << "Failed to map source surface for premultiplying alpha.";
        return nullptr;
      }

      dstSurface = Factory::CreateDataSourceSurface(srcSurface->GetSize(), srcSurface->GetFormat());

      if (!dstSurface || !dstSurface->Map(DataSourceSurface::MapType::WRITE, &dstMap)) {
        gfxCriticalError() << "Failed to map destination surface for premultiplying alpha.";
        srcSurface->Unmap();
        return nullptr;
      }
    }

    uint8_t rIndex = 0;
    uint8_t gIndex = 0;
    uint8_t bIndex = 0;
    uint8_t aIndex = 0;

    if (mSurface->GetFormat() == SurfaceFormat::R8G8B8A8) {
      rIndex = 0;
      gIndex = 1;
      bIndex = 2;
      aIndex = 3;
    } else if (mSurface->GetFormat() == SurfaceFormat::B8G8R8A8) {
      rIndex = 2;
      gIndex = 1;
      bIndex = 0;
      aIndex = 3;
    } else if (mSurface->GetFormat() == SurfaceFormat::A8R8G8B8) {
      rIndex = 1;
      gIndex = 2;
      bIndex = 3;
      aIndex = 0;
    }

    for (int i = 0; i < dstSurface->GetSize().height; ++i) {
      uint8_t* bufferPtr = dstMap.mData + dstMap.mStride * i;
      uint8_t* srcBufferPtr = srcSurface ? srcMap.mData + srcMap.mStride * i : bufferPtr;
      for (int i = 0; i < dstSurface->GetSize().width; ++i) {
        uint8_t r = *(srcBufferPtr+rIndex);
        uint8_t g = *(srcBufferPtr+gIndex);
        uint8_t b = *(srcBufferPtr+bIndex);
        uint8_t a = *(srcBufferPtr+aIndex);

        *(bufferPtr+rIndex) = gfxUtils::sPremultiplyTable[a * 256 + r];
        *(bufferPtr+gIndex) = gfxUtils::sPremultiplyTable[a * 256 + g];
        *(bufferPtr+bIndex) = gfxUtils::sPremultiplyTable[a * 256 + b];
        *(bufferPtr+aIndex) = a;

        bufferPtr += 4;
        srcBufferPtr += 4;
      }
    }

    dstSurface->Unmap();
    if (srcSurface) {
      srcSurface->Unmap();
    }

    mSurface = dstSurface;
  }

  // Replace our surface with one optimized for the target we're about to draw
  // to, under the assumption it'll likely be drawn again to that target.
  // This call should be a no-op for already-optimized surfaces
  mSurface = target->OptimizeSourceSurface(mSurface);

  RefPtr<gfx::SourceSurface> surface(mSurface);
  return surface.forget();
}

already_AddRefed<layers::Image>
ImageBitmap::TransferAsImage()
{
  RefPtr<layers::Image> image = mData;
  Close();
  return image.forget();
}

UniquePtr<ImageBitmapCloneData>
ImageBitmap::ToCloneData() const
{
  UniquePtr<ImageBitmapCloneData> result(new ImageBitmapCloneData());
  result->mPictureRect = mPictureRect;
  result->mIsPremultipliedAlpha = mIsPremultipliedAlpha;
  RefPtr<SourceSurface> surface = mData->GetAsSourceSurface();
  result->mSurface = surface->GetDataSurface();
  MOZ_ASSERT(result->mSurface);
  result->mWriteOnly = mWriteOnly;

  return Move(result);
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateFromCloneData(nsIGlobalObject* aGlobal,
                                 ImageBitmapCloneData* aData)
{
  RefPtr<layers::Image> data = CreateImageFromSurface(aData->mSurface);

  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal, data, aData->mWriteOnly,
                                            aData->mIsPremultipliedAlpha);

  // Report memory allocation.
  RegisterAllocation(aGlobal, aData->mSurface);

  ErrorResult rv;
  ret->SetPictureRect(aData->mPictureRect, rv);
  return ret.forget();
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateFromOffscreenCanvas(nsIGlobalObject* aGlobal,
                                       OffscreenCanvas& aOffscreenCanvas,
                                       ErrorResult& aRv)
{
  // Check origin-clean
  bool writeOnly = aOffscreenCanvas.IsWriteOnly();

  nsLayoutUtils::SurfaceFromElementResult res =
    nsLayoutUtils::SurfaceFromOffscreenCanvas(&aOffscreenCanvas,
                                              nsLayoutUtils::SFE_WANT_FIRST_FRAME);

  RefPtr<SourceSurface> surface = res.GetSourceSurface();

  if (NS_WARN_IF(!surface)) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  RefPtr<layers::Image> data =
    CreateImageFromSurface(surface);

  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal, data, writeOnly);

  // Report memory allocation.
  RegisterAllocation(aGlobal, surface);

  return ret.forget();
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateInternal(nsIGlobalObject* aGlobal, HTMLImageElement& aImageEl,
                            const Maybe<IntRect>& aCropRect, ErrorResult& aRv)
{
  // Check if the image element is completely available or not.
  if (!aImageEl.Complete()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }
  
  // Check if the image element is a bitmap (e.g. it's a vector graphic) or not.
  if (!HasRasterImage(aImageEl)) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  bool writeOnly = true;

  // Get the SourceSurface out from the image element and then do security
  // checking.
  RefPtr<SourceSurface> surface = GetSurfaceFromElement(aGlobal, aImageEl,
                                                        &writeOnly, aRv);

  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  // Create ImageBitmap.
  RefPtr<layers::Image> data = CreateImageFromSurface(surface);

  if (NS_WARN_IF(!data)) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal, data, writeOnly);

  // Set the picture rectangle.
  if (ret && aCropRect.isSome()) {
    ret->SetPictureRect(aCropRect.ref(), aRv);
  }

  return ret.forget();
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateInternal(nsIGlobalObject* aGlobal, HTMLVideoElement& aVideoEl,
                            const Maybe<IntRect>& aCropRect, ErrorResult& aRv)
{
  aVideoEl.MarkAsContentSource(mozilla::dom::HTMLVideoElement::CallerAPI::CREATE_IMAGEBITMAP);

  // Check network state.
  if (aVideoEl.NetworkState() == HTMLMediaElement::NETWORK_EMPTY) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  // Check ready state.
  // Cannot be HTMLMediaElement::HAVE_NOTHING or HTMLMediaElement::HAVE_METADATA.
  if (aVideoEl.ReadyState() <= HTMLMediaElement::HAVE_METADATA) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  bool writeOnly = true;
  
  // Check security.
  nsCOMPtr<nsIPrincipal> principal = aVideoEl.GetCurrentVideoPrincipal();
  bool CORSUsed = aVideoEl.GetCORSMode() != CORS_NONE;

  writeOnly = !CheckSecurityForHTMLElements(false, CORSUsed, principal);

  // Create ImageBitmap.
  ImageContainer *container = aVideoEl.GetImageContainer();

  if (!container) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  AutoLockImage lockImage(container);
  layers::Image* data = lockImage.GetImage();
  if (!data) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }
  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal, data, writeOnly);

  // Set the picture rectangle.
  if (ret && aCropRect.isSome()) {
    ret->SetPictureRect(aCropRect.ref(), aRv);
  }

  return ret.forget();
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateInternal(nsIGlobalObject* aGlobal, HTMLCanvasElement& aCanvasEl,
                            const Maybe<IntRect>& aCropRect, ErrorResult& aRv)
{
  if (aCanvasEl.Width() == 0 || aCanvasEl.Height() == 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  bool writeOnly = true;
  
  RefPtr<SourceSurface> surface = GetSurfaceFromElement(aGlobal, aCanvasEl, &writeOnly, aRv);

  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  if (!writeOnly) {
    writeOnly = aCanvasEl.IsWriteOnly();
  }

  // Crop the source surface if needed.
  RefPtr<SourceSurface> croppedSurface;
  IntRect cropRect = aCropRect.valueOr(IntRect());

  // If the HTMLCanvasElement's rendering context is WebGL, then the snapshot
  // we got from the HTMLCanvasElement is a DataSourceSurface which is a copy
  // of the rendering context. We handle cropping in this case.
  bool needToReportMemoryAllocation = false;
  if ((aCanvasEl.GetCurrentContextType() == CanvasContextType::WebGL1 ||
       aCanvasEl.GetCurrentContextType() == CanvasContextType::WebGL2) &&
      aCropRect.isSome()) {
    // The _surface_ must be a DataSourceSurface.
    MOZ_ASSERT(surface->GetType() == SurfaceType::DATA,
               "The snapshot SourceSurface from WebGL rendering contest is not DataSourceSurface.");
    RefPtr<DataSourceSurface> dataSurface = surface->GetDataSurface();
    croppedSurface = CropAndCopyDataSourceSurface(dataSurface, cropRect);
    cropRect.MoveTo(0, 0);
    needToReportMemoryAllocation = true;
  }
  else {
    croppedSurface = surface;
  }

  if (NS_WARN_IF(!croppedSurface)) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  // Create an Image from the SourceSurface.
  RefPtr<layers::Image> data = CreateImageFromSurface(croppedSurface);

  if (NS_WARN_IF(!data)) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal, data, writeOnly);

  // Report memory allocation if needed.
  if (needToReportMemoryAllocation) {
    RegisterAllocation(aGlobal, croppedSurface);
  }

  // Set the picture rectangle.
  if (ret && aCropRect.isSome()) {
    ret->SetPictureRect(cropRect, aRv);
  }

  return ret.forget();
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateInternal(nsIGlobalObject* aGlobal, ImageData& aImageData,
                            const Maybe<IntRect>& aCropRect, ErrorResult& aRv)
{
  // Copy data into SourceSurface.
  RootedSpiderMonkeyInterface<Uint8ClampedArray> array(RootingCx());
  DebugOnly<bool> inited = array.Init(aImageData.GetDataObject());
  MOZ_ASSERT(inited);

  array.ComputeLengthAndData();
  const SurfaceFormat FORMAT = SurfaceFormat::R8G8B8A8;
  const uint32_t BYTES_PER_PIXEL = BytesPerPixel(FORMAT);
  const uint32_t imageWidth = aImageData.Width();
  const uint32_t imageHeight = aImageData.Height();
  const uint32_t imageStride = imageWidth * BYTES_PER_PIXEL;
  const uint32_t dataLength = array.Length();
  const gfx::IntSize imageSize(imageWidth, imageHeight);

  // Check the ImageData is neutered or not.
  if (imageWidth == 0 || imageHeight == 0 ||
      (imageWidth * imageHeight * BYTES_PER_PIXEL) != dataLength) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  // Create and Crop the raw data into a layers::Image
  RefPtr<layers::Image> data;

  // The data could move during a GC; copy it out into a local buffer.
  uint8_t* fixedData = array.Data();

  if (NS_IsMainThread()) {
    data = CreateImageFromRawData(imageSize, imageStride, FORMAT,
                                  fixedData, dataLength,
                                  aCropRect);
  } else {
    RefPtr<CreateImageFromRawDataInMainThreadSyncTask> task
      = new CreateImageFromRawDataInMainThreadSyncTask(fixedData,
                                                       dataLength,
                                                       imageStride,
                                                       FORMAT,
                                                       imageSize,
                                                       aCropRect,
                                                       getter_AddRefs(data));
    task->Dispatch(Terminating, aRv);
  }

  if (NS_WARN_IF(!data)) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  // Create an ImageBitmap.
  // ImageData's underlying data is not alpha-premultiplied.
  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal,
                                            data,
                                            false /* write-only */,
                                            false /* alpha-premult */);

  // Report memory allocation.
  RegisterAllocation(aGlobal, data);

  // The cropping information has been handled in the CreateImageFromRawData()
  // function.

  return ret.forget();
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateInternal(nsIGlobalObject* aGlobal, CanvasRenderingContext2D& aCanvasCtx,
                            const Maybe<IntRect>& aCropRect, ErrorResult& aRv)
{
  // Check origin-clean
  bool writeOnly = aCanvasCtx.GetCanvas()->IsWriteOnly() || aCanvasCtx.IsWriteOnly();

  RefPtr<SourceSurface> surface = aCanvasCtx.GetSurfaceSnapshot();

  if (NS_WARN_IF(!surface)) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  const IntSize surfaceSize = surface->GetSize();
  if (surfaceSize.width == 0 || surfaceSize.height == 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  RefPtr<layers::Image> data = CreateImageFromSurface(surface);

  if (NS_WARN_IF(!data)) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal, data, writeOnly);

  // Report memory allocation.
  RegisterAllocation(aGlobal, surface);

  // Set the picture rectangle.
  if (ret && aCropRect.isSome()) {
    ret->SetPictureRect(aCropRect.ref(), aRv);
  }

  return ret.forget();
}

/* static */ already_AddRefed<ImageBitmap>
ImageBitmap::CreateInternal(nsIGlobalObject* aGlobal, ImageBitmap& aImageBitmap,
                            const Maybe<IntRect>& aCropRect, ErrorResult& aRv)
{
  if (!aImageBitmap.mData) {
    aRv.Throw(NS_ERROR_NOT_AVAILABLE);
    return nullptr;
  }

  RefPtr<layers::Image> data = aImageBitmap.mData;
  RefPtr<ImageBitmap> ret = new ImageBitmap(aGlobal,
                                            data,
                                            aImageBitmap.mWriteOnly,
                                            aImageBitmap.mIsPremultipliedAlpha);

  // Set the picture rectangle.
  if (ret && aCropRect.isSome()) {
    ret->SetPictureRect(aCropRect.ref(), aRv);
  }

  return ret.forget();
}

class FulfillImageBitmapPromise
{
protected:
  FulfillImageBitmapPromise(Promise* aPromise, ImageBitmap* aImageBitmap)
  : mPromise(aPromise)
  , mImageBitmap(aImageBitmap)
  {
    MOZ_ASSERT(aPromise);
  }

  void DoFulfillImageBitmapPromise()
  {
    mPromise->MaybeResolve(mImageBitmap);
  }

private:
  RefPtr<Promise> mPromise;
  RefPtr<ImageBitmap> mImageBitmap;
};

class FulfillImageBitmapPromiseTask final : public Runnable,
                                            public FulfillImageBitmapPromise
{
public:
  FulfillImageBitmapPromiseTask(Promise* aPromise, ImageBitmap* aImageBitmap)
  : FulfillImageBitmapPromise(aPromise, aImageBitmap)
  {
  }

  NS_IMETHOD Run() override
  {
    DoFulfillImageBitmapPromise();
    return NS_OK;
  }
};

class FulfillImageBitmapPromiseWorkerTask final : public WorkerSameThreadRunnable,
                                                  public FulfillImageBitmapPromise
{
public:
  FulfillImageBitmapPromiseWorkerTask(Promise* aPromise, ImageBitmap* aImageBitmap)
  : WorkerSameThreadRunnable(GetCurrentThreadWorkerPrivate()),
    FulfillImageBitmapPromise(aPromise, aImageBitmap)
  {
  }

  bool WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    DoFulfillImageBitmapPromise();
    return true;
  }
};

static void
AsyncFulfillImageBitmapPromise(Promise* aPromise, ImageBitmap* aImageBitmap)
{
  if (NS_IsMainThread()) {
    nsCOMPtr<nsIRunnable> task =
      new FulfillImageBitmapPromiseTask(aPromise, aImageBitmap);
    NS_DispatchToCurrentThread(task); // Actually, to the main-thread.
  } else {
    RefPtr<FulfillImageBitmapPromiseWorkerTask> task =
      new FulfillImageBitmapPromiseWorkerTask(aPromise, aImageBitmap);
    task->Dispatch(); // Actually, to the current worker-thread.
  }
}

static already_AddRefed<SourceSurface>
DecodeBlob(Blob& aBlob)
{
  // Get the internal stream of the blob.
  nsCOMPtr<nsIInputStream> stream;
  ErrorResult error;
  aBlob.Impl()->GetInternalStream(getter_AddRefs(stream), error);
  if (NS_WARN_IF(error.Failed())) {
    error.SuppressException();
    return nullptr;
  }

  // Get the MIME type string of the blob.
  // The type will be checked in the DecodeImage() method.
  nsAutoString mimeTypeUTF16;
  aBlob.GetType(mimeTypeUTF16);

  // Get the Component object.
  nsCOMPtr<imgITools> imgtool = do_GetService(NS_IMGTOOLS_CID);
  if (NS_WARN_IF(!imgtool)) {
    return nullptr;
  }

  // Decode image.
  NS_ConvertUTF16toUTF8 mimeTypeUTF8(mimeTypeUTF16); // NS_ConvertUTF16toUTF8 ---|> nsAutoCString
  nsCOMPtr<imgIContainer> imgContainer;
  nsresult rv = imgtool->DecodeImage(stream, mimeTypeUTF8, getter_AddRefs(imgContainer));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  // Get the surface out.
  uint32_t frameFlags = imgIContainer::FLAG_SYNC_DECODE | imgIContainer::FLAG_WANT_DATA_SURFACE;
  uint32_t whichFrame = imgIContainer::FRAME_FIRST;
  RefPtr<SourceSurface> surface = imgContainer->GetFrame(whichFrame, frameFlags);

  if (NS_WARN_IF(!surface)) {
    return nullptr;
  }

  return surface.forget();
}

static already_AddRefed<layers::Image>
DecodeAndCropBlob(Blob& aBlob, Maybe<IntRect>& aCropRect,
                  /*Output*/ IntSize& sourceSize)
{
  // Decode the blob into a SourceSurface.
  RefPtr<SourceSurface> surface = DecodeBlob(aBlob);

  if (NS_WARN_IF(!surface)) {
    return nullptr;
  }

  // Set the _sourceSize_ output parameter.
  sourceSize = surface->GetSize();

  // Crop the source surface if needed.
  RefPtr<SourceSurface> croppedSurface = surface;

  if (aCropRect.isSome()) {
    // The blob is just decoded into a RasterImage and not optimized yet, so the
    // _surface_ we get is a DataSourceSurface which wraps the RasterImage's
    // raw buffer.
    //
    // The _surface_ might already be optimized so that its type is not
    // SurfaceType::DATA. However, we could keep using the generic cropping and
    // copying since the decoded buffer is only used in this ImageBitmap so we
    // should crop it to save memory usage.
    //
    // TODO: Bug1189632 is going to refactor this create-from-blob part to
    //       decode the blob off the main thread. Re-check if we should do
    //       cropping at this moment again there.
    RefPtr<DataSourceSurface> dataSurface = surface->GetDataSurface();
    croppedSurface = CropAndCopyDataSourceSurface(dataSurface, aCropRect.ref());
    aCropRect->MoveTo(0, 0);
  }

  if (NS_WARN_IF(!croppedSurface)) {
    return nullptr;
  }

  // Create an Image from the source surface.
  RefPtr<layers::Image> image = CreateImageFromSurface(croppedSurface);

  if (NS_WARN_IF(!image)) {
    return nullptr;
  }

  return image.forget();
}

class CreateImageBitmapFromBlob
{
protected:
  CreateImageBitmapFromBlob(Promise* aPromise,
                            nsIGlobalObject* aGlobal,
                            Blob& aBlob,
                            const Maybe<IntRect>& aCropRect)
  : mPromise(aPromise),
    mGlobalObject(aGlobal),
    mBlob(&aBlob),
    mCropRect(aCropRect)
  {
  }

  virtual ~CreateImageBitmapFromBlob()
  {
  }

  // Returns true on success, false on failure.
  bool DoCreateImageBitmapFromBlob()
  {
    RefPtr<ImageBitmap> imageBitmap = CreateImageBitmap();

    // handle errors while creating ImageBitmap
    // (1) error occurs during reading of the object
    // (2) the image data is not in a supported file format
    // (3) the image data is corrupted
    // All these three cases should reject the promise with "InvalidStateError"
    // DOMException
    if (!imageBitmap) {
      return false;
    }

    if (imageBitmap && mCropRect.isSome()) {
      ErrorResult rv;
      imageBitmap->SetPictureRect(mCropRect.ref(), rv);

      if (rv.Failed()) {
        mPromise->MaybeReject(rv);
        return false;
      }
    }

    // Report memory allocation.
    RegisterAllocation(mGlobalObject, imageBitmap->mData);

    mPromise->MaybeResolve(imageBitmap);
    return true;
  }

  // Will return null on failure.  In that case, mPromise will already
  // be rejected with the right thing.
  virtual already_AddRefed<ImageBitmap> CreateImageBitmap() = 0;

  RefPtr<Promise> mPromise;
  nsCOMPtr<nsIGlobalObject> mGlobalObject;
  RefPtr<mozilla::dom::Blob> mBlob;
  Maybe<IntRect> mCropRect;
};

class CreateImageBitmapFromBlobTask final : public Runnable,
                                            public CreateImageBitmapFromBlob
{
public:
  CreateImageBitmapFromBlobTask(Promise* aPromise,
                                nsIGlobalObject* aGlobal,
                                Blob& aBlob,
                                const Maybe<IntRect>& aCropRect)
  :CreateImageBitmapFromBlob(aPromise, aGlobal, aBlob, aCropRect)
  {
  }

  NS_IMETHOD Run() override
  {
    DoCreateImageBitmapFromBlob();
    return NS_OK;
  }

private:
  already_AddRefed<ImageBitmap> CreateImageBitmap() override
  {
    // _sourceSize_ is used to get the original size of the source image,
    // before being cropped.
    IntSize sourceSize;

    // Keep the orignal cropping rectangle because the mCropRect might be
    // modified in DecodeAndCropBlob().
    Maybe<IntRect> originalCropRect = mCropRect;

    RefPtr<layers::Image> data = DecodeAndCropBlob(*mBlob, mCropRect, sourceSize);

    if (NS_WARN_IF(!data)) {
      mPromise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }

    // Create ImageBitmap object.
    RefPtr<ImageBitmap> imageBitmap = new ImageBitmap(mGlobalObject, data, false /* write-only */);

    return imageBitmap.forget();
  }
};

class CreateImageBitmapFromBlobWorkerTask final : public WorkerSameThreadRunnable,
                                                  public CreateImageBitmapFromBlob
{
  // This is a synchronous task.
  class DecodeBlobInMainThreadSyncTask final : public WorkerMainThreadRunnable
  {
  public:
    DecodeBlobInMainThreadSyncTask(WorkerPrivate* aWorkerPrivate,
                                   Blob& aBlob,
                                   Maybe<IntRect>& aCropRect,
                                   layers::Image** aImage,
                                   IntSize& aSourceSize)
    : WorkerMainThreadRunnable(aWorkerPrivate,
                               NS_LITERAL_CSTRING("ImageBitmap :: Create Image from Blob"))
    , mBlob(aBlob)
    , mCropRect(aCropRect)
    , mImage(aImage)
    , mSourceSize(aSourceSize)
    {
    }

    bool MainThreadRun() override
    {
      RefPtr<layers::Image> image = DecodeAndCropBlob(mBlob, mCropRect, mSourceSize);

      if (NS_WARN_IF(!image)) {
        return true;
      }

      image.forget(mImage);

      return true;
    }

  private:
    Blob& mBlob;
    Maybe<IntRect>& mCropRect;
    layers::Image** mImage;
    IntSize mSourceSize;
  };

public:
  CreateImageBitmapFromBlobWorkerTask(Promise* aPromise,
                                  nsIGlobalObject* aGlobal,
                                  mozilla::dom::Blob& aBlob,
                                  const Maybe<IntRect>& aCropRect)
  : WorkerSameThreadRunnable(GetCurrentThreadWorkerPrivate()),
    CreateImageBitmapFromBlob(aPromise, aGlobal, aBlob, aCropRect)
  {
  }

  bool WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    return DoCreateImageBitmapFromBlob();
  }

private:
  already_AddRefed<ImageBitmap> CreateImageBitmap() override
  {
    // _sourceSize_ is used to get the original size of the source image,
    // before being cropped.
    IntSize sourceSize;

    // Keep the orignal cropping rectangle because the mCropRect might be
    // modified in DecodeAndCropBlob().
    Maybe<IntRect> originalCropRect = mCropRect;

    RefPtr<layers::Image> data;

    ErrorResult rv;
    RefPtr<DecodeBlobInMainThreadSyncTask> task =
      new DecodeBlobInMainThreadSyncTask(mWorkerPrivate, *mBlob, mCropRect,
                                         getter_AddRefs(data), sourceSize);
    task->Dispatch(Terminating, rv); // This is a synchronous call.

    // In case the worker is terminating, this rejection can be handled.
    if (NS_WARN_IF(rv.Failed())) {
      mPromise->MaybeReject(rv);
      return nullptr;
    }

    if (NS_WARN_IF(!data)) {
      mPromise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }

    // Create ImageBitmap object.
    RefPtr<ImageBitmap> imageBitmap = new ImageBitmap(mGlobalObject, data, false /* write-only */);

    return imageBitmap.forget();
  }

};

static void
AsyncCreateImageBitmapFromBlob(Promise* aPromise, nsIGlobalObject* aGlobal,
                               Blob& aBlob, const Maybe<IntRect>& aCropRect)
{
  if (NS_IsMainThread()) {
    nsCOMPtr<nsIRunnable> task =
      new CreateImageBitmapFromBlobTask(aPromise, aGlobal, aBlob, aCropRect);
    NS_DispatchToCurrentThread(task); // Actually, to the main-thread.
  } else {
    RefPtr<CreateImageBitmapFromBlobWorkerTask> task =
      new CreateImageBitmapFromBlobWorkerTask(aPromise, aGlobal, aBlob, aCropRect);
    task->Dispatch(); // Actually, to the current worker-thread.
  }
}

/* static */ already_AddRefed<Promise>
ImageBitmap::Create(nsIGlobalObject* aGlobal, const ImageBitmapSource& aSrc,
                    const Maybe<gfx::IntRect>& aCropRect, ErrorResult& aRv)
{
  MOZ_ASSERT(aGlobal);

  RefPtr<Promise> promise = Promise::Create(aGlobal, aRv);

  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  if (aCropRect.isSome() && (aCropRect->Width() == 0 || aCropRect->Height() == 0)) {
    aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return promise.forget();
  }

  RefPtr<ImageBitmap> imageBitmap;

  if (aSrc.IsHTMLImageElement()) {
    MOZ_ASSERT(NS_IsMainThread(),
               "Creating ImageBitmap from HTMLImageElement off the main thread.");
    imageBitmap = CreateInternal(aGlobal, aSrc.GetAsHTMLImageElement(), aCropRect, aRv);
  } else if (aSrc.IsHTMLVideoElement()) {
    MOZ_ASSERT(NS_IsMainThread(),
               "Creating ImageBitmap from HTMLVideoElement off the main thread.");
    imageBitmap = CreateInternal(aGlobal, aSrc.GetAsHTMLVideoElement(), aCropRect, aRv);
  } else if (aSrc.IsHTMLCanvasElement()) {
    MOZ_ASSERT(NS_IsMainThread(),
               "Creating ImageBitmap from HTMLCanvasElement off the main thread.");
    imageBitmap = CreateInternal(aGlobal, aSrc.GetAsHTMLCanvasElement(), aCropRect, aRv);
  } else if (aSrc.IsImageData()) {
    imageBitmap = CreateInternal(aGlobal, aSrc.GetAsImageData(), aCropRect, aRv);
  } else if (aSrc.IsCanvasRenderingContext2D()) {
    MOZ_ASSERT(NS_IsMainThread(),
               "Creating ImageBitmap from CanvasRenderingContext2D off the main thread.");
    imageBitmap = CreateInternal(aGlobal, aSrc.GetAsCanvasRenderingContext2D(), aCropRect, aRv);
  } else if (aSrc.IsImageBitmap()) {
    imageBitmap = CreateInternal(aGlobal, aSrc.GetAsImageBitmap(), aCropRect, aRv);
  } else if (aSrc.IsBlob()) {
    AsyncCreateImageBitmapFromBlob(promise, aGlobal, aSrc.GetAsBlob(), aCropRect);
    return promise.forget();
  } else {
    aRv.Throw(NS_ERROR_NOT_IMPLEMENTED);
  }

  if (!aRv.Failed()) {
    AsyncFulfillImageBitmapPromise(promise, imageBitmap);
  }

  return promise.forget();
}

/*static*/ JSObject*
ImageBitmap::ReadStructuredClone(JSContext* aCx,
                                 JSStructuredCloneReader* aReader,
                                 nsIGlobalObject* aParent,
                                 const nsTArray<RefPtr<DataSourceSurface>>& aClonedSurfaces,
                                 uint32_t aIndex)
{
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aReader);
  // aParent might be null.

  uint32_t picRectX_;
  uint32_t picRectY_;
  uint32_t picRectWidth_;
  uint32_t picRectHeight_;
  uint32_t isPremultipliedAlpha_;
  uint32_t writeOnly;
  uint32_t dummy;

  if (!JS_ReadUint32Pair(aReader, &picRectX_, &picRectY_) ||
      !JS_ReadUint32Pair(aReader, &picRectWidth_, &picRectHeight_) ||
      !JS_ReadUint32Pair(aReader, &isPremultipliedAlpha_,
                                  &dummy) ||
      !JS_ReadUint32Pair(aReader, &writeOnly, &dummy)) {
    return nullptr;
  }

  int32_t picRectX = BitwiseCast<int32_t>(picRectX_);
  int32_t picRectY = BitwiseCast<int32_t>(picRectY_);
  int32_t picRectWidth = BitwiseCast<int32_t>(picRectWidth_);
  int32_t picRectHeight = BitwiseCast<int32_t>(picRectHeight_);

  // Create a new ImageBitmap.
  MOZ_ASSERT(!aClonedSurfaces.IsEmpty());
  MOZ_ASSERT(aIndex < aClonedSurfaces.Length());

  // RefPtr<ImageBitmap> needs to go out of scope before toObjectOrNull() is
  // called because the static analysis thinks dereferencing XPCOM objects
  // can GC (because in some cases it can!), and a return statement with a
  // JSObject* type means that JSObject* is on the stack as a raw pointer
  // while destructors are running.
  JS::Rooted<JS::Value> value(aCx);
  {
    RefPtr<layers::Image> img = CreateImageFromSurface(aClonedSurfaces[aIndex]);
    RefPtr<ImageBitmap> imageBitmap =
      new ImageBitmap(aParent, img, !!writeOnly, isPremultipliedAlpha_);

    ErrorResult error;
    imageBitmap->SetPictureRect(IntRect(picRectX, picRectY,
                                        picRectWidth, picRectHeight), error);
    if (NS_WARN_IF(error.Failed())) {
      error.SuppressException();
      return nullptr;
    }

    if (!GetOrCreateDOMReflector(aCx, imageBitmap, &value)) {
      return nullptr;
    }

    // Report memory allocation.
    RegisterAllocation(aParent, aClonedSurfaces[aIndex]);
  }

  return &(value.toObject());
}

/*static*/ bool
ImageBitmap::WriteStructuredClone(JSStructuredCloneWriter* aWriter,
                                  nsTArray<RefPtr<DataSourceSurface>>& aClonedSurfaces,
                                  ImageBitmap* aImageBitmap)
{
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aImageBitmap);

  const uint32_t picRectX = BitwiseCast<uint32_t>(aImageBitmap->mPictureRect.x);
  const uint32_t picRectY = BitwiseCast<uint32_t>(aImageBitmap->mPictureRect.y);
  const uint32_t picRectWidth = BitwiseCast<uint32_t>(aImageBitmap->mPictureRect.width);
  const uint32_t picRectHeight = BitwiseCast<uint32_t>(aImageBitmap->mPictureRect.height);
  const uint32_t isPremultipliedAlpha = aImageBitmap->mIsPremultipliedAlpha ? 1 : 0;
  const uint32_t isWriteOnly = aImageBitmap->mWriteOnly ? 1 : 0;

  // Indexing the cloned surfaces and send the index to the receiver.
  uint32_t index = aClonedSurfaces.Length();

  if (NS_WARN_IF(!JS_WriteUint32Pair(aWriter, SCTAG_DOM_IMAGEBITMAP, index)) ||
      NS_WARN_IF(!JS_WriteUint32Pair(aWriter, picRectX, picRectY)) ||
      NS_WARN_IF(!JS_WriteUint32Pair(aWriter, picRectWidth, picRectHeight)) ||
      NS_WARN_IF(!JS_WriteUint32Pair(aWriter, isPremultipliedAlpha, 0)) ||
      NS_WARN_IF(!JS_WriteUint32Pair(aWriter, isWriteOnly, 0))) {
    return false;
  }

  RefPtr<SourceSurface> surface =
    aImageBitmap->mData->GetAsSourceSurface();
  RefPtr<DataSourceSurface> snapshot = surface->GetDataSurface();
  RefPtr<DataSourceSurface> dstDataSurface;
  {
    // DataSourceSurfaceD2D1::GetStride() will call EnsureMapped implicitly and
    // won't Unmap after exiting function. So instead calling GetStride()
    // directly, using ScopedMap to get stride.
    DataSourceSurface::ScopedMap map(snapshot, DataSourceSurface::READ);
    dstDataSurface =
      Factory::CreateDataSourceSurfaceWithStride(snapshot->GetSize(),
                                                 snapshot->GetFormat(),
                                                 map.GetStride(),
                                                 true);
  }
  MOZ_ASSERT(dstDataSurface);
  Factory::CopyDataSourceSurface(snapshot, dstDataSurface);
  aClonedSurfaces.AppendElement(dstDataSurface);
  return true;
}

} // namespace dom
} // namespace mozilla
