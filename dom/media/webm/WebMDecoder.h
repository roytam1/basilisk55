/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(WebMDecoder_h_)
#define WebMDecoder_h_

#include "MediaDecoder.h"
#include "MediaFormatReader.h"

namespace mozilla {

class MediaContainerType;

class WebMDecoder : public MediaDecoder
{
public:
  explicit WebMDecoder(MediaDecoderOwner* aOwner) : MediaDecoder(aOwner) {}
  MediaDecoder* Clone(MediaDecoderOwner* aOwner) override {
    if (!IsWebMEnabled()) {
      return nullptr;
    }
    return new WebMDecoder(aOwner);
  }
  MediaDecoderStateMachine* CreateStateMachine() override;

  // Returns true if aContainerType is a WebM type that we think we can render
  // with an enabled platform decoder backend.
  // If provided, codecs are checked for support.
  static bool IsSupportedType(const MediaContainerType& aContainerType);

  void GetMozDebugReaderData(nsACString& aString) override;

private:
  RefPtr<MediaFormatReader> mReader;
};

} // namespace mozilla

#endif
