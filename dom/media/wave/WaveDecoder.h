/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(WaveDecoder_h_)
#define WaveDecoder_h_

#include "MediaDecoder.h"

namespace mozilla {

class MediaContainerType;

class WaveDecoder : public MediaDecoder
{
public:
  // MediaDecoder interface.
  explicit WaveDecoder(MediaDecoderOwner* aOwner) : MediaDecoder(aOwner) {}
  MediaDecoder* Clone(MediaDecoderOwner* aOwner) override;
  MediaDecoderStateMachine* CreateStateMachine() override;

  // Returns true if the Wave backend is pref'ed on, and we're running on a
  // platform that is likely to have decoders for the format.
  static bool IsSupportedType(const MediaContainerType& aContainerType);
};

} // namespace mozilla

#endif
