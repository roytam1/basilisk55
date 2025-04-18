/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VideoUtils.h"

#include "mozilla/Base64.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/Telemetry.h"

#include "MediaContainerType.h"
#include "MediaPrefs.h"
#include "MediaResource.h"
#include "TimeUnits.h"
#include "nsMathUtils.h"
#include "nsSize.h"
#include "VorbisUtils.h"
#include "ImageContainer.h"
#include "mozilla/SharedThreadPool.h"
#include "nsIRandomGenerator.h"
#include "nsIServiceManager.h"
#include "nsServiceManagerUtils.h"
#include "nsIConsoleService.h"
#include "nsThreadUtils.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsContentTypeParser.h"

#include <functional>
#include <stdint.h>

namespace mozilla {

NS_NAMED_LITERAL_CSTRING(kEMEKeySystemClearkey, "org.w3.clearkey");
NS_NAMED_LITERAL_CSTRING(kEMEKeySystemWidevine, "com.widevine.alpha");
NS_NAMED_LITERAL_CSTRING(kEMEKeySystemPrimetime, "com.adobe.primetime");

using layers::PlanarYCbCrImage;

CheckedInt64 SaferMultDiv(int64_t aValue, uint32_t aMul, uint32_t aDiv) {
  int64_t major = aValue / aDiv;
  int64_t remainder = aValue % aDiv;
  return CheckedInt64(remainder) * aMul / aDiv + CheckedInt64(major) * aMul;
}

// Converts from number of audio frames to microseconds, given the specified
// audio rate.
CheckedInt64 FramesToUsecs(int64_t aFrames, uint32_t aRate) {
  return SaferMultDiv(aFrames, USECS_PER_S, aRate);
}

media::TimeUnit FramesToTimeUnit(int64_t aFrames, uint32_t aRate) {
  int64_t major = aFrames / aRate;
  int64_t remainder = aFrames % aRate;
  return media::TimeUnit::FromMicroseconds(major) * USECS_PER_S +
    (media::TimeUnit::FromMicroseconds(remainder) * USECS_PER_S) / aRate;
}

// Converts from microseconds to number of audio frames, given the specified
// audio rate.
CheckedInt64 UsecsToFrames(int64_t aUsecs, uint32_t aRate) {
  return SaferMultDiv(aUsecs, aRate, USECS_PER_S);
}

// Format TimeUnit as number of frames at given rate.
CheckedInt64 TimeUnitToFrames(const media::TimeUnit& aTime, uint32_t aRate) {
  return UsecsToFrames(aTime.ToMicroseconds(), aRate);
}

nsresult SecondsToUsecs(double aSeconds, int64_t& aOutUsecs) {
  if (aSeconds * double(USECS_PER_S) > INT64_MAX) {
    return NS_ERROR_FAILURE;
  }
  aOutUsecs = int64_t(aSeconds * double(USECS_PER_S));
  return NS_OK;
}

static int32_t ConditionDimension(float aValue)
{
  // This will exclude NaNs and too-big values.
  if (aValue > 1.0 && aValue <= INT32_MAX)
    return int32_t(NS_round(aValue));
  return 0;
}

void ScaleDisplayByAspectRatio(nsIntSize& aDisplay, float aAspectRatio)
{
  if (aAspectRatio > 1.0) {
    // Increase the intrinsic width
    aDisplay.width = ConditionDimension(aAspectRatio * aDisplay.width);
  } else {
    // Increase the intrinsic height
    aDisplay.height = ConditionDimension(aDisplay.height / aAspectRatio);
  }
}

static int64_t BytesToTime(int64_t offset, int64_t length, int64_t durationUs) {
  NS_ASSERTION(length > 0, "Must have positive length");
  double r = double(offset) / double(length);
  if (r > 1.0)
    r = 1.0;
  return int64_t(double(durationUs) * r);
}

media::TimeIntervals GetEstimatedBufferedTimeRanges(mozilla::MediaResource* aStream,
                                                    int64_t aDurationUsecs)
{
  media::TimeIntervals buffered;
  // Nothing to cache if the media takes 0us to play.
  if (aDurationUsecs <= 0 || !aStream)
    return buffered;

  // Special case completely cached files.  This also handles local files.
  if (aStream->IsDataCachedToEndOfResource(0)) {
    buffered +=
      media::TimeInterval(media::TimeUnit::FromMicroseconds(0),
                          media::TimeUnit::FromMicroseconds(aDurationUsecs));
    return buffered;
  }

  int64_t totalBytes = aStream->GetLength();

  // If we can't determine the total size, pretend that we have nothing
  // buffered. This will put us in a state of eternally-low-on-undecoded-data
  // which is not great, but about the best we can do.
  if (totalBytes <= 0)
    return buffered;

  int64_t startOffset = aStream->GetNextCachedData(0);
  while (startOffset >= 0) {
    int64_t endOffset = aStream->GetCachedDataEnd(startOffset);
    // Bytes [startOffset..endOffset] are cached.
    NS_ASSERTION(startOffset >= 0, "Integer underflow in GetBuffered");
    NS_ASSERTION(endOffset >= 0, "Integer underflow in GetBuffered");

    int64_t startUs = BytesToTime(startOffset, totalBytes, aDurationUsecs);
    int64_t endUs = BytesToTime(endOffset, totalBytes, aDurationUsecs);
    if (startUs != endUs) {
      buffered +=
        media::TimeInterval(media::TimeUnit::FromMicroseconds(startUs),

                              media::TimeUnit::FromMicroseconds(endUs));
    }
    startOffset = aStream->GetNextCachedData(endOffset);
  }
  return buffered;
}

void DownmixStereoToMono(mozilla::AudioDataValue* aBuffer,
                         uint32_t aFrames)
{
  MOZ_ASSERT(aBuffer);
  const int channels = 2;
  for (uint32_t fIdx = 0; fIdx < aFrames; ++fIdx) {
#ifdef MOZ_SAMPLE_TYPE_FLOAT32
    float sample = 0.0;
#else
    int sample = 0;
#endif
    // The sample of the buffer would be interleaved.
    sample = (aBuffer[fIdx*channels] + aBuffer[fIdx*channels + 1]) * 0.5;
    aBuffer[fIdx*channels] = aBuffer[fIdx*channels + 1] = sample;
  }
}

bool
IsVideoContentType(const nsCString& aContentType)
{
  NS_NAMED_LITERAL_CSTRING(video, "video");
  if (FindInReadable(video, aContentType)) {
    return true;
  }
  return false;
}

bool
IsValidVideoRegion(const nsIntSize& aFrame, const nsIntRect& aPicture,
                   const nsIntSize& aDisplay)
{
  return
    aFrame.width <= PlanarYCbCrImage::MAX_DIMENSION &&
    aFrame.height <= PlanarYCbCrImage::MAX_DIMENSION &&
    aFrame.width * aFrame.height <= MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
    aFrame.width * aFrame.height != 0 &&
    aPicture.width <= PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.x < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.x + aPicture.width < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.height <= PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.y < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.y + aPicture.height < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.width * aPicture.height <= MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
    aPicture.width * aPicture.height != 0 &&
    aDisplay.width <= PlanarYCbCrImage::MAX_DIMENSION &&
    aDisplay.height <= PlanarYCbCrImage::MAX_DIMENSION &&
    aDisplay.width * aDisplay.height <= MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
    aDisplay.width * aDisplay.height != 0;
}

already_AddRefed<SharedThreadPool> GetMediaThreadPool(MediaThreadType aType)
{
  const char *name;
  switch (aType) {
    case MediaThreadType::PLATFORM_DECODER:
      name = "MediaPDecoder";
      break;
    default:
      MOZ_FALLTHROUGH_ASSERT("Unexpected MediaThreadType");
    case MediaThreadType::PLAYBACK:
      name = "MediaPlayback";
      break;
  }
  RefPtr<SharedThreadPool> pool = SharedThreadPool::
    Get(nsDependentCString(name), MediaPrefs::MediaThreadPoolDefaultCount());

  // Ensure a larger stack for platform decoder threads
  if (aType == MediaThreadType::PLATFORM_DECODER) {
    const uint32_t minStackSize = 512*1024;
    uint32_t stackSize;
    MOZ_ALWAYS_SUCCEEDS(pool->GetThreadStackSize(&stackSize));
    if (stackSize < minStackSize) {
      MOZ_ALWAYS_SUCCEEDS(pool->SetThreadStackSize(minStackSize));
    }
  }

  return pool.forget();
}

bool
ExtractVPXCodecDetails(const nsAString& aCodec,
                       uint8_t& aProfile,
                       uint8_t& aLevel,
                       uint8_t& aBitDepth)
{
  uint8_t dummyChromaSubsampling = 1;
  VideoColorSpace dummyColorspace;
  return ExtractVPXCodecDetails(aCodec,
                                aProfile,
                                aLevel,
                                aBitDepth,
                                dummyChromaSubsampling,
                                dummyColorspace);
}

bool ExtractVPXCodecDetails(const nsAString& aCodec,
                            uint8_t& aProfile,
                            uint8_t& aLevel,
                            uint8_t& aBitDepth,
                            uint8_t& aChromaSubsampling,
                            VideoColorSpace& aColorSpace)
{
  nsTArray<nsString> fieldsArr;

  // Assign default value.
  aChromaSubsampling = 1;
  
  nsCharSeparatedTokenizer tokenizer(aCodec, '.');
  while (tokenizer.hasMoreTokens()) {
    const nsSubstring& token = tokenizer.nextToken();
    fieldsArr.AppendElement(token);
  }
  auto fourCC = fieldsArr[0];

  if (!fourCC.EqualsLiteral("vp09") && !fourCC.EqualsLiteral("vp08")) {
    // Invalid 4CC
    return false;
  }
  uint8_t *fields[] = { &aProfile, &aLevel, &aBitDepth, &aChromaSubsampling,
                        &aColorSpace.mPrimaryId, &aColorSpace.mTransferId,
                        &aColorSpace.mMatrixId, &aColorSpace.mRangeId };
  int fieldsCount = 0;
  nsresult rv;
  for (int fieldsItr = 1; fieldsItr < fieldsArr.Length(); ++fieldsItr, ++fieldsCount) {
    if (fieldsCount > 7) {
      // No more than 8 fields are expected.
      return false;
    }
    *(fields[fieldsCount]) =
      static_cast<uint8_t>(fieldsArr[fieldsItr].ToInteger(&rv));
    // We got invalid field value, parsing error.
    NS_ENSURE_SUCCESS(rv, false);
  }
  // Mandatory Fields
  // <sample entry 4CC>.<profile>.<level>.<bitDepth>.
  // Optional Fields
  // <chromaSubsampling>.<colourPrimaries>.<transferCharacteristics>.
  // <matrixCoefficients>.<videoFullRangeFlag>
  // First three fields are mandatory(we have parsed 4CC).
  if (fieldsCount < 3) {
    // Invalid number of fields.
    return false;
  }
  // Start to validate the parsing value.

  // profile should be 0,1,2 or 3.
  // See https://www.webmproject.org/vp9/profiles/
  // We don't support more than profile 2
  if (aProfile > 2) {
    // Invalid profile.
    return false;
  }

 // level, See https://www.webmproject.org/vp9/mp4/#semantics_1
  switch (aLevel) {
    case 10:
    case 11:
    case 20:
    case 21:
    case 30:
    case 31:
    case 40:
    case 41:
    case 50:
    case 51:
    case 52:
    case 60:
    case 61:
    case 62:
      break;
    default:
      // Invalid level.
      return false;
  }

  if (aBitDepth != 8 && aBitDepth != 10 && aBitDepth != 12) {
    // Invalid bitDepth:
    return false;
  }

  if (fieldsCount == 3) {
    // No more options.
    return true;
  }

  // chromaSubsampling should be 0,1,2,3...4~7 are reserved.
  if (aChromaSubsampling > 3) {
    return false;
  }

  if (fieldsCount == 4) {
    // No more options.
    return true;
  }

  // It is an integer that is defined by the "Colour primaries"
  // section of ISO/IEC 23001-8:2016 Table 2.
  // We treat reserved value as false case.
  const auto& primaryId = aColorSpace.mPrimaryId;
  if (primaryId == 0 || primaryId == 3 || primaryId > 22) {
    // reserved value.
    return false;
  }
  if (primaryId > 12 && primaryId < 22) {
    // 13~21 are reserved values.
    return false;
  }

  if (fieldsCount == 5) {
    // No more options.
    return true;
  }

  // It is an integer that is defined by the
  // "Transfer characteristics" section of ISO/IEC 23001-8:2016 Table 3.
  // We treat reserved value as false case.
  const auto& transferId = aColorSpace.mTransferId;
  if (transferId == 0 || transferId == 3 || transferId > 18) {
    // reserved value.
    return false;
  }

  if (fieldsCount == 6) {
    // No more options.
    return true;
  }

  // It is an integer that is defined by the
  // "Matrix coefficients" section of ISO/IEC 23001-8:2016 Table 4.
  // We treat reserved value as false case.
  const auto& matrixId = aColorSpace.mMatrixId;
  if (matrixId == 3 || matrixId > 11) {
    return false;
  }

  // If matrixCoefficients is 0 (RGB), then chroma subsampling MUST be 3 (4:4:4).
  if (matrixId == 0 && aChromaSubsampling != 3) {
    return false;
  }

  if (fieldsCount == 7) {
    // No more options.
    return true;
  }

  // videoFullRangeFlag indicates the black level and range of the luma and
  // chroma signals. 0 = legal range (e.g. 16-235 for 8 bit sample depth);
  // 1 = full range (e.g. 0-255 for 8-bit sample depth).
  const auto& rangeId = aColorSpace.mRangeId;
  return rangeId <= 1;
}

bool
ExtractH264CodecDetails(const nsAString& aCodec,
                        int16_t& aProfile,
                        int16_t& aLevel)
{
  // H.264 codecs parameters have a type defined as avcN.PPCCLL, where
  // N = avc type. avc3 is avcc with SPS & PPS implicit (within stream)
  // PP = profile_idc, CC = constraint_set flags, LL = level_idc.
  // We ignore the constraint_set flags, as it's not clear from any
  // documentation what constraints the platform decoders support.
  // See http://blog.pearce.org.nz/2013/11/what-does-h264avc1-codecs-parameters.html
  // for more details.
  if (aCodec.Length() != strlen("avc1.PPCCLL")) {
    return false;
  }

  // Verify the codec starts with "avc1." or "avc3.".
  const nsAString& sample = Substring(aCodec, 0, 5);
  if (!sample.EqualsASCII("avc1.") && !sample.EqualsASCII("avc3.")) {
    return false;
  }

  // Extract the profile_idc and level_idc.
  nsresult rv = NS_OK;
  aProfile = PromiseFlatString(Substring(aCodec, 5, 2)).ToInteger(&rv, 16);
  NS_ENSURE_SUCCESS(rv, false);

  aLevel = PromiseFlatString(Substring(aCodec, 9, 2)).ToInteger(&rv, 16);
  NS_ENSURE_SUCCESS(rv, false);

  if (aLevel == 9) {
    aLevel = H264_LEVEL_1_b;
  } else if (aLevel <= 5) {
    aLevel *= 10;
  }

  // Capture the constraint_set flag value for the purpose of Telemetry.
  // We don't NS_ENSURE_SUCCESS here because ExtractH264CodecDetails doesn't
  // care about this, but we make sure constraints is above 4 (constraint_set5_flag)
  // otherwise collect 0 for unknown.
  uint8_t constraints = PromiseFlatString(Substring(aCodec, 7, 2)).ToInteger(&rv, 16);
  Telemetry::Accumulate(Telemetry::VIDEO_CANPLAYTYPE_H264_CONSTRAINT_SET_FLAG,
                        constraints >= 4 ? constraints : 0);

  // 244 is the highest meaningful profile value (High 4:4:4 Intra Profile)
  // that can be represented as single hex byte, otherwise collect 0 for unknown.
  Telemetry::Accumulate(Telemetry::VIDEO_CANPLAYTYPE_H264_PROFILE,
                        aProfile <= 244 ? aProfile : 0);

  // Make sure aLevel represents a value between levels 1 and 5.2,
  // otherwise collect 0 for unknown.
  Telemetry::Accumulate(Telemetry::VIDEO_CANPLAYTYPE_H264_LEVEL,
                        (aLevel >= 10 && aLevel <= 52) ? aLevel : 0);

  return true;
}

nsresult
GenerateRandomName(nsCString& aOutSalt, uint32_t aLength)
{
  nsresult rv;
  nsCOMPtr<nsIRandomGenerator> rg =
    do_GetService("@mozilla.org/security/random-generator;1", &rv);
  if (NS_FAILED(rv)) return rv;

  // For each three bytes of random data we will get four bytes of ASCII.
  const uint32_t requiredBytesLength =
    static_cast<uint32_t>((aLength + 3) / 4 * 3);

  uint8_t* buffer;
  rv = rg->GenerateRandomBytes(requiredBytesLength, &buffer);
  if (NS_FAILED(rv)) return rv;

  nsAutoCString temp;
  nsDependentCSubstring randomData(reinterpret_cast<const char*>(buffer),
                                   requiredBytesLength);
  rv = Base64Encode(randomData, temp);
  free(buffer);
  buffer = nullptr;
  if (NS_FAILED (rv)) return rv;

  aOutSalt = temp;
  return NS_OK;
}

nsresult
GenerateRandomPathName(nsCString& aOutSalt, uint32_t aLength)
{
  nsresult rv = GenerateRandomName(aOutSalt, aLength);
  if (NS_FAILED(rv)) return rv;

  // Base64 characters are alphanumeric (a-zA-Z0-9) and '+' and '/', so we need
  // to replace illegal characters -- notably '/'
  aOutSalt.ReplaceChar(FILE_PATH_SEPARATOR FILE_ILLEGAL_CHARACTERS, '_');
  return NS_OK;
}

already_AddRefed<TaskQueue>
CreateMediaDecodeTaskQueue()
{
  RefPtr<TaskQueue> queue = new TaskQueue(
    GetMediaThreadPool(MediaThreadType::PLATFORM_DECODER));
  return queue.forget();
}

void
SimpleTimer::Cancel() {
  if (mTimer) {
#ifdef DEBUG
    nsCOMPtr<nsIEventTarget> target;
    mTimer->GetTarget(getter_AddRefs(target));
    nsCOMPtr<nsIThread> thread(do_QueryInterface(target));
    MOZ_ASSERT(NS_GetCurrentThread() == thread);
#endif
    mTimer->Cancel();
    mTimer = nullptr;
  }
  mTask = nullptr;
}

NS_IMETHODIMP
SimpleTimer::Notify(nsITimer *timer) {
  RefPtr<SimpleTimer> deathGrip(this);
  if (mTask) {
    mTask->Run();
    mTask = nullptr;
  }
  return NS_OK;
}

nsresult
SimpleTimer::Init(nsIRunnable* aTask, uint32_t aTimeoutMs, nsIThread* aTarget)
{
  nsresult rv;

  // Get target thread first, so we don't have to cancel the timer if it fails.
  nsCOMPtr<nsIThread> target;
  if (aTarget) {
    target = aTarget;
  } else {
    rv = NS_GetMainThread(getter_AddRefs(target));
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  nsCOMPtr<nsITimer> timer = do_CreateInstance(NS_TIMER_CONTRACTID, &rv);
  if (NS_FAILED(rv)) {
    return rv;
  }
  // Note: set target before InitWithCallback in case the timer fires before
  // we change the event target.
  rv = timer->SetTarget(aTarget);
  if (NS_FAILED(rv)) {
    timer->Cancel();
    return rv;
  }
  rv = timer->InitWithCallback(this, aTimeoutMs, nsITimer::TYPE_ONE_SHOT);
  if (NS_FAILED(rv)) {
    return rv;
  }

  mTimer = timer.forget();
  mTask = aTask;
  return NS_OK;
}

NS_IMPL_ISUPPORTS(SimpleTimer, nsITimerCallback)

already_AddRefed<SimpleTimer>
SimpleTimer::Create(nsIRunnable* aTask, uint32_t aTimeoutMs, nsIThread* aTarget)
{
  RefPtr<SimpleTimer> t(new SimpleTimer());
  if (NS_FAILED(t->Init(aTask, aTimeoutMs, aTarget))) {
    return nullptr;
  }
  return t.forget();
}

void
LogToBrowserConsole(const nsAString& aMsg)
{
  if (!NS_IsMainThread()) {
    nsString msg(aMsg);
    nsCOMPtr<nsIRunnable> task =
      NS_NewRunnableFunction([msg]() { LogToBrowserConsole(msg); });
    NS_DispatchToMainThread(task.forget(), NS_DISPATCH_NORMAL);
    return;
  }
  nsCOMPtr<nsIConsoleService> console(
    do_GetService("@mozilla.org/consoleservice;1"));
  if (!console) {
    NS_WARNING("Failed to log message to console.");
    return;
  }
  nsAutoString msg(aMsg);
  console->LogStringMessage(msg.get());
}

bool
ParseCodecsString(const nsAString& aCodecs, nsTArray<nsString>& aOutCodecs)
{
  aOutCodecs.Clear();
  bool expectMoreTokens = false;
  nsCharSeparatedTokenizer tokenizer(aCodecs, ',');
  while (tokenizer.hasMoreTokens()) {
    const nsSubstring& token = tokenizer.nextToken();
    expectMoreTokens = tokenizer.separatorAfterCurrentToken();
    aOutCodecs.AppendElement(token);
  }
  if (expectMoreTokens) {
    // Last codec name was empty
    return false;
  }
  return true;
}

bool
ParseMIMETypeString(const nsAString& aMIMEType,
                    nsString& aOutContainerType,
                    nsTArray<nsString>& aOutCodecs)
{
  nsContentTypeParser parser(aMIMEType);
  nsresult rv = parser.GetType(aOutContainerType);
  if (NS_FAILED(rv)) {
    return false;
  }

  nsString codecsStr;
  parser.GetParameter("codecs", codecsStr);
  return ParseCodecsString(codecsStr, aOutCodecs);
}

template <int N>
static bool
StartsWith(const nsACString& string, const char (&prefix)[N])
{
    if (N - 1 > string.Length()) {
      return false;
    }
    return memcmp(string.Data(), prefix, N - 1) == 0;
}

bool
IsH264CodecString(const nsAString& aCodec)
{
  int16_t profile = 0;
  int16_t level = 0;
  return ExtractH264CodecDetails(aCodec, profile, level);
}

bool
IsAACCodecString(const nsAString& aCodec)
{
  return
    aCodec.EqualsLiteral("mp4a.40.2") || // MPEG4 AAC-LC
    aCodec.EqualsLiteral("mp4a.40.5") || // MPEG4 HE-AAC
    aCodec.EqualsLiteral("mp4a.67") || // MPEG2 AAC-LC
    aCodec.EqualsLiteral("mp4a.40.29");  // MPEG4 HE-AACv2
}

bool
IsVP8CodecString(const nsAString& aCodec)
{
  uint8_t profile = 0;
  uint8_t level = 0;
  uint8_t bitDepth = 0;
  return aCodec.EqualsLiteral("vp8") ||
         aCodec.EqualsLiteral("vp8.0") ||
         (StartsWith(NS_ConvertUTF16toUTF8(aCodec), "vp08") &&
          ExtractVPXCodecDetails(aCodec, profile, level, bitDepth));
}

bool
IsVP9CodecString(const nsAString& aCodec)
{
  uint8_t profile = 0;
  uint8_t level = 0;
  uint8_t bitDepth = 0;
  return aCodec.EqualsLiteral("vp9") ||
         aCodec.EqualsLiteral("vp9.0") ||
         (StartsWith(NS_ConvertUTF16toUTF8(aCodec), "vp09") &&
          ExtractVPXCodecDetails(aCodec, profile, level, bitDepth));
}

#ifdef MOZ_AV1
bool
IsAV1CodecString(const nsAString& aCodec)
{
  return aCodec.EqualsLiteral("av1") ||
    StartsWith(NS_ConvertUTF16toUTF8(aCodec), "av01");
}
#endif

UniquePtr<TrackInfo>
CreateTrackInfoWithMIMEType(const nsACString& aCodecMIMEType)
{
  UniquePtr<TrackInfo> trackInfo;
  if (StartsWith(aCodecMIMEType, "audio/")) {
    trackInfo.reset(new AudioInfo());
    trackInfo->mMimeType = aCodecMIMEType;
  } else if (StartsWith(aCodecMIMEType, "video/")) {
    trackInfo.reset(new VideoInfo());
    trackInfo->mMimeType = aCodecMIMEType;
  }
  return trackInfo;
}

UniquePtr<TrackInfo>
CreateTrackInfoWithMIMETypeAndContainerTypeExtraParameters(
  const nsACString& aCodecMIMEType,
  const MediaContainerType& aContainerType)
{
  UniquePtr<TrackInfo> trackInfo = CreateTrackInfoWithMIMEType(aCodecMIMEType);
  if (trackInfo) {
    VideoInfo* videoInfo = trackInfo->GetAsVideoInfo();
    if (videoInfo) {
      Maybe<int32_t> maybeWidth = aContainerType.ExtendedType().GetWidth();
      if (maybeWidth && *maybeWidth > 0) {
        videoInfo->mImage.width = *maybeWidth;
      }
      Maybe<int32_t> maybeHeight = aContainerType.ExtendedType().GetHeight();
      if (maybeHeight && *maybeHeight > 0) {
        videoInfo->mImage.height = *maybeHeight;
      }
    }
  }
  return trackInfo;
}

} // end namespace mozilla
