/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DecoderTraits.h"
#include "MediaContainerType.h"
#include "MediaDecoder.h"
#include "nsMimeTypes.h"
#include "mozilla/Preferences.h"
#include "mozilla/Telemetry.h"

#include "OggDecoder.h"
#include "OggDemuxer.h"

#include "WebMDecoder.h"
#include "WebMDemuxer.h"

#ifdef MOZ_ANDROID_OMX
#include "AndroidMediaDecoder.h"
#include "AndroidMediaReader.h"
#include "AndroidMediaPluginHost.h"
#endif
#ifdef MOZ_DIRECTSHOW
#include "DirectShowDecoder.h"
#include "DirectShowReader.h"
#endif
#ifdef MOZ_FMP4
#include "MP4Decoder.h"
#include "MP4Demuxer.h"
#endif
#include "MediaFormatReader.h"

#include "MP3Decoder.h"
#include "MP3Demuxer.h"

#include "WaveDecoder.h"
#include "WaveDemuxer.h"

#include "ADTSDecoder.h"
#include "ADTSDemuxer.h"

#include "FlacDecoder.h"
#include "FlacDemuxer.h"

#include "nsPluginHost.h"
#include "MediaPrefs.h"

namespace mozilla
{

static bool
IsHttpLiveStreamingType(const MediaContainerType& aType)
{
  return // For m3u8.
         // https://tools.ietf.org/html/draft-pantos-http-live-streaming-19#section-10
         aType.Type() == MEDIAMIMETYPE("application/vnd.apple.mpegurl")
         // Some sites serve these as the informal m3u type.
         || aType.Type() == MEDIAMIMETYPE("application/x-mpegurl")
         || aType.Type() == MEDIAMIMETYPE("audio/x-mpegurl");
}

#ifdef MOZ_ANDROID_OMX
static bool
IsAndroidMediaType(const MediaContainerType& aType)
{
  if (!MediaDecoder::IsAndroidMediaPluginEnabled()) {
    return false;
  }

  return aType.Type() == MEDIAMIMETYPE("audio/mpeg")
         || aType.Type() == MEDIAMIMETYPE("audio/mp4")
         || aType.Type() == MEDIAMIMETYPE("video/mp4")
         || aType.Type() == MEDIAMIMETYPE("video/x-m4v");
}
#endif

/* static */ bool
DecoderTraits::IsMP4SupportedType(const MediaContainerType& aType,
                                  DecoderDoctorDiagnostics* aDiagnostics)
{
#ifdef MOZ_FMP4
  return MP4Decoder::IsSupportedType(aType, aDiagnostics);
#else
  return false;
#endif
}

static
CanPlayStatus
CanHandleCodecsType(const MediaContainerType& aType,
                    DecoderDoctorDiagnostics* aDiagnostics)
{
  // We should have been given a codecs string, though it may be empty.
  MOZ_ASSERT(aType.ExtendedType().HaveCodecs());

  // Container type with the MIME type, no codecs.
  const MediaContainerType mimeType(aType.Type());

  if (OggDecoder::IsSupportedType(mimeType)) {
    if (OggDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    } else {
      // We can only reach this position if a particular codec was requested,
      // ogg is supported and working: the codec must be invalid.
      return CANPLAY_NO;
    }
  }
  if (WaveDecoder::IsSupportedType(MediaContainerType(mimeType))) {
    if (WaveDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    } else {
      // We can only reach this position if a particular codec was requested,
      // ogg is supported and working: the codec must be invalid.
      return CANPLAY_NO;
    }
  }
#if !defined(MOZ_OMX_WEBM_DECODER)
  if (WebMDecoder::IsSupportedType(mimeType)) {
    if (WebMDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    } else {
      // We can only reach this position if a particular codec was requested,
      // webm is supported and working: the codec must be invalid.
      return CANPLAY_NO;
    }
  }
#endif
#ifdef MOZ_FMP4
  if (MP4Decoder::IsSupportedType(mimeType,
                                  /* DecoderDoctorDiagnostics* */ nullptr)) {
    if (MP4Decoder::IsSupportedType(aType, aDiagnostics)) {
      return CANPLAY_YES;
    } else {
      // We can only reach this position if a particular codec was requested,
      // fmp4 is supported and working: the codec must be invalid.
      return CANPLAY_NO;
    }
  }
#endif
  if (MP3Decoder::IsSupportedType(aType)) {
    return CANPLAY_YES;
  }
  if (ADTSDecoder::IsSupportedType(aType)) {
    return CANPLAY_YES;
  }
  if (FlacDecoder::IsSupportedType(aType)) {
    return CANPLAY_YES;
  }

  MediaCodecs supportedCodecs;
#ifdef MOZ_DIRECTSHOW
  DirectShowDecoder::GetSupportedCodecs(aType, &supportedCodecs);
#endif
#ifdef MOZ_ANDROID_OMX
  if (MediaDecoder::IsAndroidMediaPluginEnabled()) {
    EnsureAndroidMediaPluginHost()->FindDecoder(aType, &supportedCodecs);
  }
#endif
  if (supportedCodecs.IsEmpty()) {
    return CANPLAY_MAYBE;
  }

  if (!supportedCodecs.ContainsAll(aType.ExtendedType().Codecs())) {
    // At least one requested codec is not supported.
    return CANPLAY_NO;
  }

  return CANPLAY_YES;
}

static
CanPlayStatus
CanHandleMediaType(const MediaContainerType& aType,
                   DecoderDoctorDiagnostics* aDiagnostics)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (IsHttpLiveStreamingType(aType)) {
    Telemetry::Accumulate(Telemetry::MEDIA_HLS_CANPLAY_REQUESTED, true);
  }

  if (aType.ExtendedType().HaveCodecs()) {
    CanPlayStatus result = CanHandleCodecsType(aType, aDiagnostics);
    if (result == CANPLAY_NO || result == CANPLAY_YES) {
      return result;
    }
  }

  // Container type with just the MIME type/subtype, no codecs.
  const MediaContainerType mimeType(aType.Type());

  if (OggDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (WaveDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
#ifdef MOZ_FMP4
  if (MP4Decoder::IsSupportedType(mimeType, aDiagnostics)) {
    return CANPLAY_MAYBE;
  }
#endif
#if !defined(MOZ_OMX_WEBM_DECODER)
  if (WebMDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
#endif
  if (MP3Decoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (ADTSDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (FlacDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
#ifdef MOZ_DIRECTSHOW
  if (DirectShowDecoder::GetSupportedCodecs(mimeType, nullptr)) {
    return CANPLAY_MAYBE;
  }
#endif
#ifdef MOZ_ANDROID_OMX
  if (MediaDecoder::IsAndroidMediaPluginEnabled() &&
      EnsureAndroidMediaPluginHost()->FindDecoder(mimeType, nullptr)) {
    return CANPLAY_MAYBE;
  }
#endif
  return CANPLAY_NO;
}

/* static */
CanPlayStatus
DecoderTraits::CanHandleContainerType(const MediaContainerType& aContainerType,
                                      DecoderDoctorDiagnostics* aDiagnostics)
{
  return CanHandleMediaType(aContainerType, aDiagnostics);
}

/* static */
bool DecoderTraits::ShouldHandleMediaType(const char* aMIMEType,
                                          DecoderDoctorDiagnostics* aDiagnostics)
{
  Maybe<MediaContainerType> containerType = MakeMediaContainerType(aMIMEType);
  if (!containerType) {
    return false;
  }

  // Prior to Issue #1977, we always pop-up the download prompt when
  // wave files are opened directly. This was considered inconsistent
  // since we play wave files when they're inside an HTML5 audio tag
  // anyway. However, there may be users who depended on this old
  // behavior, where they use their helper apps to open WAV audio
  // instead. We should allow this old behavior behind a pref for
  // those who want it.
  if (!Preferences::GetBool("media.wave.play-stand-alone", true) &&
      WaveDecoder::IsSupportedType(*containerType)) {
    return false;
  }

  // If an external plugin which can handle quicktime video is available
  // (and not disabled), prefer it over native playback as there are
  // several codecs found in the wild that we do not handle.
  if (containerType->Type() == MEDIAMIMETYPE("video/quicktime")) {
    RefPtr<nsPluginHost> pluginHost = nsPluginHost::GetInst();
    if (pluginHost &&
        pluginHost->HavePluginForType(containerType->Type().AsString())) {
      return false;
    }
  }

  return CanHandleMediaType(*containerType, aDiagnostics) != CANPLAY_NO;
}

// Instantiates but does not initialize decoder.
static
already_AddRefed<MediaDecoder>
InstantiateDecoder(const MediaContainerType& aType,
                   MediaDecoderOwner* aOwner,
                   DecoderDoctorDiagnostics* aDiagnostics)
{
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<MediaDecoder> decoder;

#ifdef MOZ_FMP4
  if (MP4Decoder::IsSupportedType(aType, aDiagnostics)) {
    decoder = new MP4Decoder(aOwner);
    return decoder.forget();
  }
#endif
  if (MP3Decoder::IsSupportedType(aType)) {
    decoder = new MP3Decoder(aOwner);
    return decoder.forget();
  }
  if (ADTSDecoder::IsSupportedType(aType)) {
    decoder = new ADTSDecoder(aOwner);
    return decoder.forget();
  }
  if (OggDecoder::IsSupportedType(aType)) {
    decoder = new OggDecoder(aOwner);
    return decoder.forget();
  }
  if (WaveDecoder::IsSupportedType(aType)) {
    decoder = new WaveDecoder(aOwner);
    return decoder.forget();
  }
  if (FlacDecoder::IsSupportedType(aType)) {
    decoder = new FlacDecoder(aOwner);
    return decoder.forget();
  }
#ifdef MOZ_ANDROID_OMX
  if (MediaDecoder::IsAndroidMediaPluginEnabled() &&
      EnsureAndroidMediaPluginHost()->FindDecoder(aType, nullptr)) {
    decoder = new AndroidMediaDecoder(aOwner, aType);
    return decoder.forget();
  }
#endif

  if (WebMDecoder::IsSupportedType(aType)) {
    decoder = new WebMDecoder(aOwner);
    return decoder.forget();
  }

#ifdef MOZ_DIRECTSHOW
  // Note: DirectShow should come before WMF, so that we prefer DirectShow's
  // MP3 support over WMF's.
  if (DirectShowDecoder::GetSupportedCodecs(aType, nullptr)) {
    decoder = new DirectShowDecoder(aOwner);
    return decoder.forget();
  }
#endif

  if (IsHttpLiveStreamingType(aType)) {
    // We don't have an HLS decoder.
    Telemetry::Accumulate(Telemetry::MEDIA_HLS_DECODER_SUCCESS, false);
  }

  return nullptr;
}

/* static */
already_AddRefed<MediaDecoder>
DecoderTraits::CreateDecoder(const nsACString& aType,
                             MediaDecoderOwner* aOwner,
                             DecoderDoctorDiagnostics* aDiagnostics)
{
  MOZ_ASSERT(NS_IsMainThread());
  Maybe<MediaContainerType> type = MakeMediaContainerType(aType);
  if (!type) {
    return nullptr;
  }
  return InstantiateDecoder(*type, aOwner, aDiagnostics);
}

/* static */
MediaDecoderReader*
DecoderTraits::CreateReader(const MediaContainerType& aType,
                            AbstractMediaDecoder* aDecoder)
{
  MOZ_ASSERT(NS_IsMainThread());
  MediaDecoderReader* decoderReader = nullptr;

  if (!aDecoder) {
    return decoderReader;
  }

#ifdef MOZ_FMP4
  if (MP4Decoder::IsSupportedType(aType,
                                  /* DecoderDoctorDiagnostics* */ nullptr)) {
    decoderReader = new MediaFormatReader(aDecoder, new MP4Demuxer(aDecoder->GetResource()));
  } else
#endif
  if (MP3Decoder::IsSupportedType(aType)) {
    decoderReader = new MediaFormatReader(aDecoder, new mp3::MP3Demuxer(aDecoder->GetResource()));
  } else
  if (ADTSDecoder::IsSupportedType(aType)) {
    decoderReader = new MediaFormatReader(aDecoder, new ADTSDemuxer(aDecoder->GetResource()));
  } else
  if (WaveDecoder::IsSupportedType(aType)) {
    decoderReader = new MediaFormatReader(aDecoder, new WAVDemuxer(aDecoder->GetResource()));
  } else
  if (FlacDecoder::IsSupportedType(aType)) {
    decoderReader = new MediaFormatReader(aDecoder, new FlacDemuxer(aDecoder->GetResource()));
  } else
  if (OggDecoder::IsSupportedType(aType)) {
    decoderReader = new MediaFormatReader(aDecoder, new OggDemuxer(aDecoder->GetResource()));
  } else
#ifdef MOZ_ANDROID_OMX
  if (MediaDecoder::IsAndroidMediaPluginEnabled() &&
      EnsureAndroidMediaPluginHost()->FindDecoder(aType, nullptr)) {
    decoderReader = new AndroidMediaReader(aDecoder, aType);
  } else
#endif
  if (WebMDecoder::IsSupportedType(aType)) {
    decoderReader =
      new MediaFormatReader(aDecoder, new WebMDemuxer(aDecoder->GetResource()));
  } else
#ifdef MOZ_DIRECTSHOW
  if (DirectShowDecoder::GetSupportedCodecs(aType, nullptr)) {
    decoderReader = new DirectShowReader(aDecoder);
  } else
#endif
  if (false) {} // dummy if to take care of the dangling else

  return decoderReader;
}

/* static */
bool DecoderTraits::IsSupportedInVideoDocument(const nsACString& aType)
{
  // Forbid playing media in video documents if the user has opted
  // not to, using either the legacy WMF specific pref, or the newer
  // catch-all pref.
  if (!Preferences::GetBool("media.windows-media-foundation.play-stand-alone", true) ||
      !Preferences::GetBool("media.play-stand-alone", true)) {
    return false;
  }

  Maybe<MediaContainerType> type = MakeMediaContainerType(aType);
  if (!type) {
    return false;
  }

  return
    OggDecoder::IsSupportedType(*type) ||
    WebMDecoder::IsSupportedType(*type) ||
#ifdef MOZ_ANDROID_OMX
    (MediaDecoder::IsAndroidMediaPluginEnabled() && IsAndroidMediaType(*type)) ||
#endif
#ifdef MOZ_FMP4
    MP4Decoder::IsSupportedType(*type, /* DecoderDoctorDiagnostics* */ nullptr) ||
#endif
    MP3Decoder::IsSupportedType(*type) ||
    WaveDecoder::IsSupportedType(*type) ||
    ADTSDecoder::IsSupportedType(*type) ||
    FlacDecoder::IsSupportedType(*type) ||
#ifdef MOZ_DIRECTSHOW
    DirectShowDecoder::GetSupportedCodecs(*type, nullptr) ||
#endif
    false;
}

} // namespace mozilla
