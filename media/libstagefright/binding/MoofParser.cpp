/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mp4_demuxer/MoofParser.h"
#include "mp4_demuxer/Box.h"
#include "mp4_demuxer/SinfParser.h"
#include <limits>
#include "Intervals.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/Logging.h"

#if defined(MOZ_FMP4)
extern mozilla::LogModule* GetDemuxerLog();

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define LOG(name, arg, ...) MOZ_LOG(GetDemuxerLog(), mozilla::LogLevel::Debug, (TOSTRING(name) "(%p)::%s: " arg, this, __func__, ##__VA_ARGS__))
#else
#define LOG(...)
#endif

namespace mp4_demuxer
{

using namespace stagefright;
using namespace mozilla;

const uint32_t kKeyIdSize = 16;

bool
MoofParser::RebuildFragmentedIndex(const MediaByteRangeSet& aByteRanges)
{
  BoxContext context(mSource, aByteRanges);
  return RebuildFragmentedIndex(context);
}

bool
MoofParser::RebuildFragmentedIndex(
  const MediaByteRangeSet& aByteRanges, bool* aCanEvict)
{
  MOZ_ASSERT(aCanEvict);
  if (*aCanEvict && mMoofs.Length() > 1) {
    MOZ_ASSERT(mMoofs.Length() == mMediaRanges.Length());
    mMoofs.RemoveElementsAt(0, mMoofs.Length() - 1);
    mMediaRanges.RemoveElementsAt(0, mMediaRanges.Length() - 1);
    *aCanEvict = true;
  } else {
    *aCanEvict = false;
  }
  return RebuildFragmentedIndex(aByteRanges);
}

bool
MoofParser::RebuildFragmentedIndex(BoxContext& aContext)
{
  bool foundValidMoof = false;
  bool foundMdat = false;

  for (Box box(&aContext, mOffset); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("moov") && mInitRange.IsEmpty()) {
      mInitRange = MediaByteRange(0, box.Range().mEnd);
      ParseMoov(box);
    } else if (box.IsType("moof")) {
      Moof moof(box, mTrex, mMvhd, mMdhd, mEdts, mSinf, &mLastDecodeTime, mIsAudio);

      if (!moof.IsValid() && !box.Next().IsAvailable()) {
        // Moof isn't valid abort search for now.
        break;
      }

      if (!mMoofs.IsEmpty()) {
        // Stitch time ranges together in the case of a (hopefully small) time
        // range gap between moofs.
        mMoofs.LastElement().FixRounding(moof);
      }

      mMoofs.AppendElement(moof);
      mMediaRanges.AppendElement(moof.mRange);
      foundValidMoof = true;
    } else if (box.IsType("mdat") && !Moofs().IsEmpty()) {
      // Check if we have all our data from last moof.
      Moof& moof = Moofs().LastElement();
      media::Interval<int64_t> datarange(moof.mMdatRange.mStart, moof.mMdatRange.mEnd, 0);
      media::Interval<int64_t> mdat(box.Range().mStart, box.Range().mEnd, 0);
      if (datarange.Intersects(mdat)) {
        mMediaRanges.LastElement() =
          mMediaRanges.LastElement().Span(box.Range());
      }
    }
    mOffset = box.NextOffset();
  }
  return foundValidMoof;
}

MediaByteRange
MoofParser::FirstCompleteMediaHeader()
{
  if (Moofs().IsEmpty()) {
    return MediaByteRange();
  }
  return Moofs()[0].mRange;
}

MediaByteRange
MoofParser::FirstCompleteMediaSegment()
{
  for (uint32_t i = 0 ; i < mMediaRanges.Length(); i++) {
    if (mMediaRanges[i].Contains(Moofs()[i].mMdatRange)) {
      return mMediaRanges[i];
    }
  }
  return MediaByteRange();
}

class BlockingStream : public Stream {
public:
  explicit BlockingStream(Stream* aStream) : mStream(aStream)
  {
  }

  bool ReadAt(int64_t offset, void* data, size_t size, size_t* bytes_read)
    override
  {
    return mStream->ReadAt(offset, data, size, bytes_read);
  }

  bool CachedReadAt(int64_t offset, void* data, size_t size, size_t* bytes_read)
    override
  {
    return mStream->ReadAt(offset, data, size, bytes_read);
  }

  virtual bool Length(int64_t* size) override
  {
    return mStream->Length(size);
  }

private:
  RefPtr<Stream> mStream;
};

bool
MoofParser::BlockingReadNextMoof()
{
  int64_t length = std::numeric_limits<int64_t>::max();
  mSource->Length(&length);
  MediaByteRangeSet byteRanges;
  byteRanges += MediaByteRange(0, length);
  RefPtr<mp4_demuxer::BlockingStream> stream = new BlockingStream(mSource);

  BoxContext context(stream, byteRanges);
  for (Box box(&context, mOffset); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("moof")) {
      byteRanges.Clear();
      byteRanges += MediaByteRange(mOffset, box.Range().mEnd);
      return RebuildFragmentedIndex(context);
    }
  }
  return false;
}

void
MoofParser::ScanForMetadata(mozilla::MediaByteRange& aFtyp,
                            mozilla::MediaByteRange& aMoov)
{
  int64_t length = std::numeric_limits<int64_t>::max();
  mSource->Length(&length);
  MediaByteRangeSet byteRanges;
  byteRanges += MediaByteRange(0, length);
  RefPtr<mp4_demuxer::BlockingStream> stream = new BlockingStream(mSource);

  BoxContext context(stream, byteRanges);
  for (Box box(&context, mOffset); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("ftyp")) {
      aFtyp = box.Range();
      continue;
    }
    if (box.IsType("moov")) {
      aMoov = box.Range();
      break;
    }
  }
  mInitRange = aFtyp.Span(aMoov);
}

bool
MoofParser::HasMetadata()
{
  MediaByteRange ftyp;
  MediaByteRange moov;
  ScanForMetadata(ftyp, moov);
  return !!ftyp.Length() && !!moov.Length();
}

already_AddRefed<mozilla::MediaByteBuffer>
MoofParser::Metadata()
{
  MediaByteRange ftyp;
  MediaByteRange moov;
  ScanForMetadata(ftyp, moov);
  CheckedInt<MediaByteBuffer::size_type> ftypLength = ftyp.Length();
  CheckedInt<MediaByteBuffer::size_type> moovLength = moov.Length();
  if (!ftypLength.isValid() || !moovLength.isValid()
      || !ftypLength.value() || !moovLength.value()) {
    // No ftyp or moov, or they cannot be used as array size.
    return nullptr;
  }
  CheckedInt<MediaByteBuffer::size_type> totalLength = ftypLength + moovLength;
  if (!totalLength.isValid()) {
    // Addition overflow, or sum cannot be used as array size.
    return nullptr;
  }
  RefPtr<MediaByteBuffer> metadata = new MediaByteBuffer();
  if (!metadata->SetLength(totalLength.value(), fallible)) {
    // OOM
    return nullptr;
  }

  RefPtr<mp4_demuxer::BlockingStream> stream = new BlockingStream(mSource);
  size_t read;
  bool rv =
    stream->ReadAt(ftyp.mStart, metadata->Elements(), ftypLength.value(), &read);
  if (!rv || read != ftypLength.value()) {
    return nullptr;
  }
  rv =
    stream->ReadAt(moov.mStart, metadata->Elements() + ftypLength.value(), moovLength.value(), &read);
  if (!rv || read != moovLength.value()) {
    return nullptr;
  }
  return metadata.forget();
}

Interval<Microseconds>
MoofParser::GetCompositionRange(const MediaByteRangeSet& aByteRanges)
{
  Interval<Microseconds> compositionRange;
  BoxContext context(mSource, aByteRanges);
  for (size_t i = 0; i < mMoofs.Length(); i++) {
    Moof& moof = mMoofs[i];
    Box box(&context, moof.mRange.mStart);
    if (box.IsAvailable()) {
      compositionRange = compositionRange.Extents(moof.mTimeRange);
    }
  }
  return compositionRange;
}

bool
MoofParser::ReachedEnd()
{
  int64_t length;
  return mSource->Length(&length) && mOffset == length;
}

void
MoofParser::ParseMoov(Box& aBox)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("mvhd")) {
      mMvhd = Mvhd(box);
    } else if (box.IsType("trak")) {
      ParseTrak(box);
    } else if (box.IsType("mvex")) {
      ParseMvex(box);
    }
  }
}

void
MoofParser::ParseTrak(Box& aBox)
{
  Tkhd tkhd;
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("tkhd")) {
      tkhd = Tkhd(box);
    } else if (box.IsType("mdia")) {
      if (!mTrex.mTrackId || tkhd.mTrackId == mTrex.mTrackId) {
        ParseMdia(box, tkhd);
      }
    } else if (box.IsType("edts") &&
               (!mTrex.mTrackId || tkhd.mTrackId == mTrex.mTrackId)) {
      mEdts = Edts(box);
    }
  }
}

void
MoofParser::ParseMdia(Box& aBox, Tkhd& aTkhd)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("mdhd")) {
      mMdhd = Mdhd(box);
    } else if (box.IsType("minf")) {
      ParseMinf(box);
    }
  }
}

void
MoofParser::ParseMvex(Box& aBox)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("trex")) {
      Trex trex = Trex(box);
      if (!mTrex.mTrackId || trex.mTrackId == mTrex.mTrackId) {
        auto trackId = mTrex.mTrackId;
        mTrex = trex;
        // Keep the original trackId, as should it be 0 we want to continue
        // parsing all tracks.
        mTrex.mTrackId = trackId;
      }
    }
  }
}

void
MoofParser::ParseMinf(Box& aBox)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("stbl")) {
      ParseStbl(box);
    }
  }
}

void
MoofParser::ParseStbl(Box& aBox)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("stsd")) {
      ParseStsd(box);
    } else if (box.IsType("sgpd")) {
      Sgpd sgpd(box);
      if (sgpd.IsValid() && sgpd.mGroupingType == "seig") {
        mTrackSampleEncryptionInfoEntries.Clear();
        mTrackSampleEncryptionInfoEntries.AppendElements(sgpd.mEntries);
      }
    } else if (box.IsType("sbgp")) {
      Sbgp sbgp(box);
      if (sbgp.IsValid() && sbgp.mGroupingType == "seig") {
        mTrackSampleToGroupEntries.Clear();
        mTrackSampleToGroupEntries.AppendElements(sbgp.mEntries);
      }
    }
  }
}

void
MoofParser::ParseStsd(Box& aBox)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("encv") || box.IsType("enca")) {
      ParseEncrypted(box);
    }
  }
}

void
MoofParser::ParseEncrypted(Box& aBox)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    // Some MP4 files have been found to have multiple sinf boxes in the same
    // enc* box. This does not match spec anyway, so just choose the first
    // one that parses properly.
    if (box.IsType("sinf")) {
      mSinf = Sinf(box);

      if (mSinf.IsValid()) {
        break;
      }
    }
  }
}

class CtsComparator
{
public:
  bool Equals(Sample* const aA, Sample* const aB) const
  {
    return aA->mCompositionRange.start == aB->mCompositionRange.start;
  }
  bool
  LessThan(Sample* const aA, Sample* const aB) const
  {
    return aA->mCompositionRange.start < aB->mCompositionRange.start;
  }
};

Moof::Moof(Box& aBox, Trex& aTrex, Mvhd& aMvhd, Mdhd& aMdhd, Edts& aEdts, Sinf& aSinf, uint64_t* aDecodeTime, bool aIsAudio)
  : mRange(aBox.Range())
  , mMaxRoundingError(35000)
{
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("traf")) {
      ParseTraf(box, aTrex, aMvhd, aMdhd, aEdts, aSinf, aDecodeTime, aIsAudio);
    }
  }
  if (IsValid()) {
    if (mIndex.Length()) {
      // Ensure the samples are contiguous with no gaps.
      nsTArray<Sample*> ctsOrder;
      for (auto& sample : mIndex) {
        ctsOrder.AppendElement(&sample);
      }
      ctsOrder.Sort(CtsComparator());

      for (size_t i = 1; i < ctsOrder.Length(); i++) {
        ctsOrder[i-1]->mCompositionRange.end = ctsOrder[i]->mCompositionRange.start;
      }

      // In MP4, the duration of a sample is defined as the delta between two decode
      // timestamps. The operation above has updated the duration of each sample
      // as a Sample's duration is mCompositionRange.end - mCompositionRange.start
      // MSE's TrackBuffersManager expects dts that increased by the sample's
      // duration, so we rewrite the dts accordingly.
      int64_t presentationDuration =
        ctsOrder.LastElement()->mCompositionRange.end
        - ctsOrder[0]->mCompositionRange.start;
      int64_t endDecodeTime =
        aMdhd.ToMicroseconds((int64_t)*aDecodeTime - aEdts.mMediaStart)
        + aMvhd.ToMicroseconds(aEdts.mEmptyOffset);
      int64_t decodeDuration = endDecodeTime - mIndex[0].mDecodeTime;
      double adjust = (double)decodeDuration / presentationDuration;
      int64_t dtsOffset = mIndex[0].mDecodeTime;
      int64_t compositionDuration = 0;
      // Adjust the dts, ensuring that the new adjusted dts will never be greater
      // than decodeTime (the next moof's decode start time).
      for (auto& sample : mIndex) {
        sample.mDecodeTime = dtsOffset + int64_t(compositionDuration * adjust);
        compositionDuration += sample.mCompositionRange.Length();
      }
      mTimeRange = Interval<Microseconds>(ctsOrder[0]->mCompositionRange.start,
          ctsOrder.LastElement()->mCompositionRange.end);
    }
    ProcessCenc();
  }
}

bool
Moof::GetAuxInfo(AtomType aType, nsTArray<MediaByteRange>* aByteRanges)
{
  aByteRanges->Clear();

  Saiz* saiz = nullptr;
  for (int i = 0; ; i++) {
    if (i == mSaizs.Length()) {
      return false;
    }
    if (mSaizs[i].mAuxInfoType == aType) {
      saiz = &mSaizs[i];
      break;
    }
  }
  Saio* saio = nullptr;
  for (int i = 0; ; i++) {
    if (i == mSaios.Length()) {
      return false;
    }
    if (mSaios[i].mAuxInfoType == aType) {
      saio = &mSaios[i];
      break;
    }
  }

  if (saio->mOffsets.Length() == 1) {
    aByteRanges->SetCapacity(saiz->mSampleInfoSize.Length());
    uint64_t offset = mRange.mStart + saio->mOffsets[0];
    for (size_t i = 0; i < saiz->mSampleInfoSize.Length(); i++) {
      aByteRanges->AppendElement(
        MediaByteRange(offset, offset + saiz->mSampleInfoSize[i]));
      offset += saiz->mSampleInfoSize[i];
    }
    return true;
  }

  if (saio->mOffsets.Length() == saiz->mSampleInfoSize.Length()) {
    aByteRanges->SetCapacity(saiz->mSampleInfoSize.Length());
    for (size_t i = 0; i < saio->mOffsets.Length(); i++) {
      uint64_t offset = mRange.mStart + saio->mOffsets[i];
      aByteRanges->AppendElement(
        MediaByteRange(offset, offset + saiz->mSampleInfoSize[i]));
    }
    return true;
  }

  return false;
}

bool
Moof::ProcessCenc()
{
  nsTArray<MediaByteRange> cencRanges;
  if (!GetAuxInfo(AtomType("cenc"), &cencRanges) ||
      cencRanges.Length() != mIndex.Length()) {
    return false;
  }
  for (int i = 0; i < cencRanges.Length(); i++) {
    mIndex[i].mCencRange = cencRanges[i];
  }
  return true;
}

void
Moof::ParseTraf(Box& aBox, Trex& aTrex, Mvhd& aMvhd, Mdhd& aMdhd, Edts& aEdts, Sinf& aSinf, uint64_t* aDecodeTime, bool aIsAudio)
{
  MOZ_ASSERT(aDecodeTime);
  Tfhd tfhd(aTrex);
  Tfdt tfdt;

  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("tfhd")) {
      tfhd = Tfhd(box, aTrex);
    } else if (!aTrex.mTrackId || tfhd.mTrackId == aTrex.mTrackId) {
      if (box.IsType("tfdt")) {
        tfdt = Tfdt(box);
      } else if (box.IsType("sgpd")) {
        Sgpd sgpd(box);
        if (sgpd.IsValid() && sgpd.mGroupingType == "seig") {
          mFragmentSampleEncryptionInfoEntries.Clear();
          mFragmentSampleEncryptionInfoEntries.AppendElements(sgpd.mEntries);
        }
      } else if (box.IsType("sbgp")) {
        Sbgp sbgp(box);
        if (sbgp.IsValid() && sbgp.mGroupingType == "seig") {
          mFragmentSampleToGroupEntries.Clear();
          mFragmentSampleToGroupEntries.AppendElements(sbgp.mEntries);
        }
      } else if (box.IsType("saiz")) {
        mSaizs.AppendElement(Saiz(box, aSinf.mDefaultEncryptionType));
      } else if (box.IsType("saio")) {
        mSaios.AppendElement(Saio(box, aSinf.mDefaultEncryptionType));
      }
    }
  }
  if (aTrex.mTrackId && tfhd.mTrackId != aTrex.mTrackId) {
    return;
  }
  // Now search for TRUN boxes.
  uint64_t decodeTime =
    tfdt.IsValid() ? tfdt.mBaseMediaDecodeTime : *aDecodeTime;
  for (Box box = aBox.FirstChild(); box.IsAvailable(); box = box.Next()) {
    if (box.IsType("trun")) {
      if (ParseTrun(box, tfhd, aMvhd, aMdhd, aEdts, &decodeTime, aIsAudio)) {
        mValid = true;
      } else {
        mValid = false;
        break;
      }
    }
  }
  *aDecodeTime = decodeTime;
}

void
Moof::FixRounding(const Moof& aMoof) {
  Microseconds gap = aMoof.mTimeRange.start - mTimeRange.end;
  if (gap > 0 && gap <= mMaxRoundingError) {
    mTimeRange.end = aMoof.mTimeRange.start;
  }
}

bool
Moof::ParseTrun(Box& aBox, Tfhd& aTfhd, Mvhd& aMvhd, Mdhd& aMdhd, Edts& aEdts, uint64_t* aDecodeTime, bool aIsAudio)
{
  if (!aTfhd.IsValid() || !aMvhd.IsValid() || !aMdhd.IsValid() ||
      !aEdts.IsValid()) {
    LOG(Moof, "Invalid dependencies: aTfhd(%d) aMvhd(%d) aMdhd(%d) aEdts(%d)",
        aTfhd.IsValid(), aMvhd.IsValid(), aMdhd.IsValid(), !aEdts.IsValid());
    return false;
  }

  BoxReader reader(aBox);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Moof, "Incomplete Box (missing flags)");
    return false;
  }
  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return false;
  }
  uint8_t version = flags >> 24;

  if (!reader->CanReadType<uint32_t>()) {
    LOG(Moof, "Incomplete Box (missing sampleCount)");
    return false;
  }
  uint32_t sampleCount;
  if (!reader->ReadU32(sampleCount)) {
    return false;
  }
  if (sampleCount == 0) {
    return true;
  }

  size_t need =
    ((flags & 1) ? sizeof(uint32_t) : 0) +
    ((flags & 4) ? sizeof(uint32_t) : 0);
  uint16_t flag[] = { 0x100, 0x200, 0x400, 0x800, 0 };
  for (size_t i = 0; flag[i]; i++) {
    if (flags & flag[i]) {
      need += sizeof(uint32_t) * sampleCount;
    }
  }
  if (reader->Remaining() < need) {
    LOG(Moof, "Incomplete Box (have:%lld need:%lld)",
        reader->Remaining(), need);
    return false;
  }

  uint64_t offset = aTfhd.mBaseDataOffset;
  if (flags & 1) {
    uint32_t baseOffset;
    if (!reader->ReadU32(baseOffset)) {
      return false;
    }
    offset += baseOffset;
  }
  uint32_t firstSampleFlags = aTfhd.mDefaultSampleFlags;
  if (flags & 4) {
    if (!reader->ReadU32(firstSampleFlags)) {
      return false;
    }
  }

  uint64_t decodeTime = *aDecodeTime;
  nsTArray<Interval<Microseconds>> timeRanges;

  if (!mIndex.SetCapacity(sampleCount, fallible)) {
    LOG(Moof, "Out of Memory");
    return false;
  }

  for (size_t i = 0; i < sampleCount; i++) {
    uint32_t sampleDuration = aTfhd.mDefaultSampleDuration;
    if (flags & 0x100) {
      if (!reader->ReadU32(sampleDuration)) {
        return false;
      }
    }
    uint32_t sampleSize = aTfhd.mDefaultSampleSize;
    if (flags & 0x200) {
      if (!reader->ReadU32(sampleSize)) {
        return false;
      }
    }
    uint32_t sampleFlags = i ? aTfhd.mDefaultSampleFlags : firstSampleFlags;
    if (flags & 0x400) {
      if (!reader->ReadU32(sampleFlags)) {
        return false;
      }
    }
    int32_t ctsOffset = 0;
    if (flags & 0x800) {
      if (!reader->Read32(ctsOffset)) {
        return false;
      }
    }

    Sample sample;
    sample.mByteRange = MediaByteRange(offset, offset + sampleSize);
    offset += sampleSize;

    sample.mDecodeTime =
      aMdhd.ToMicroseconds((int64_t)decodeTime - aEdts.mMediaStart) + aMvhd.ToMicroseconds(aEdts.mEmptyOffset);
    sample.mCompositionRange = Interval<Microseconds>(
      aMdhd.ToMicroseconds((int64_t)decodeTime + ctsOffset - aEdts.mMediaStart) + aMvhd.ToMicroseconds(aEdts.mEmptyOffset),
      aMdhd.ToMicroseconds((int64_t)decodeTime + ctsOffset + sampleDuration - aEdts.mMediaStart) + aMvhd.ToMicroseconds(aEdts.mEmptyOffset));
    decodeTime += sampleDuration;

    // Sometimes audio streams don't properly mark their samples as keyframes,
    // because every audio sample is a keyframe.
    sample.mSync = !(sampleFlags & 0x1010000) || aIsAudio;

    // FIXME: Make this infallible after bug 968520 is done.
    MOZ_ALWAYS_TRUE(mIndex.AppendElement(sample, fallible));

    mMdatRange = mMdatRange.Span(sample.mByteRange);
  }
  mMaxRoundingError += aMdhd.ToMicroseconds(sampleCount);

  *aDecodeTime = decodeTime;

  return true;
}

Tkhd::Tkhd(Box& aBox)
{
  BoxReader reader(aBox);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Tkhd, "Incomplete Box (missing flags)");
    return;
  }
  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  uint8_t version = flags >> 24;
  size_t need =
    3*(version ? sizeof(int64_t) : sizeof(int32_t)) + 2*sizeof(int32_t);
  if (reader->Remaining() < need) {
    LOG(Tkhd, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  if (version == 0) {
    uint32_t createTime, modificationTime, trackId, reserved, duration;
    if (!reader->ReadU32(createTime) ||
        !reader->ReadU32(modificationTime) ||
        !reader->ReadU32(trackId) ||
        !reader->ReadU32(reserved) ||
        !reader->ReadU32(duration)) {
      return;
    }
    NS_ASSERTION(!reserved, "reserved should be 0");
    mCreationTime = createTime;
    mModificationTime = modificationTime;
    mTrackId = trackId;
    mDuration = duration;
  } else if (version == 1) {
    uint32_t trackId, reserved;
    if (!reader->ReadU64(mCreationTime) ||
        !reader->ReadU64(mModificationTime) ||
        !reader->ReadU32(trackId) ||
        !reader->ReadU32(reserved) ||
        !reader->ReadU64(mDuration)) {
      return;
    }
    NS_ASSERTION(!reserved, "reserved should be 0");
    mTrackId = trackId;
  }
  // We don't care about whatever else may be in the box.
  mValid = true;
}

Mvhd::Mvhd(Box& aBox)
{
  BoxReader reader(aBox);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Mdhd, "Incomplete Box (missing flags)");
    return;
  }
  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  uint8_t version = flags >> 24;
  size_t need =
    3 * (version ? sizeof(int64_t) : sizeof(int32_t)) + sizeof(uint32_t);
  if (reader->Remaining() < need) {
    LOG(Mvhd, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }

  if (version == 0) {
    uint32_t createTime, modificationTime, duration;
    if (!reader->ReadU32(createTime) ||
        !reader->ReadU32(modificationTime) ||
        !reader->ReadU32(mTimescale) ||
        !reader->ReadU32(duration)) {
      return;
    }
    mCreationTime = createTime;
    mModificationTime = modificationTime;
    mDuration = duration;
  } else if (version == 1) {
    if (!reader->ReadU64(mCreationTime) ||
        !reader->ReadU64(mModificationTime) ||
        !reader->ReadU32(mTimescale) ||
        !reader->ReadU64(mDuration)) {
      return;
    }
  } else {
    return;
  }
  // We don't care about whatever else may be in the box.
  if (mTimescale) {
    mValid = true;
  }
}

Mdhd::Mdhd(Box& aBox)
  : Mvhd(aBox)
{
}

Trex::Trex(Box& aBox)
{
  BoxReader reader(aBox);
  if (reader->Remaining() < 6 * sizeof(uint32_t)) {
    LOG(Trex, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)6*sizeof(uint32_t));
    return;
  }
  if (!reader->ReadU32(mFlags) ||
      !reader->ReadU32(mTrackId) ||
      !reader->ReadU32(mDefaultSampleDescriptionIndex) ||
      !reader->ReadU32(mDefaultSampleDuration) ||
      !reader->ReadU32(mDefaultSampleSize) ||
      !reader->ReadU32(mDefaultSampleFlags)) {
    return;
  }
}

Tfhd::Tfhd(Box& aBox, Trex& aTrex)
  : Trex(aTrex)
{
  MOZ_ASSERT(aBox.IsType("tfhd"));
  MOZ_ASSERT(aBox.Parent()->IsType("traf"));
  MOZ_ASSERT(aBox.Parent()->Parent()->IsType("moof"));

  BoxReader reader(aBox);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Tfhd, "Incomplete Box (missing flags)");
    return;
  }
  if (!reader->ReadU32(mFlags)) {
    return;
  }
  size_t need = sizeof(uint32_t) /* trackid */;
  uint8_t flag[] = { 1, 2, 8, 0x10, 0x20, 0 };
  uint8_t flagSize[] = { sizeof(uint64_t), sizeof(uint32_t), sizeof(uint32_t), sizeof(uint32_t), sizeof(uint32_t) };
  for (size_t i = 0; flag[i]; i++) {
    if (mFlags & flag[i]) {
      need += flagSize[i];
    }
  }
  if (reader->Remaining() < need) {
    LOG(Tfhd, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  if (!reader->ReadU32(mTrackId)) {
    return;
  }
  mBaseDataOffset = aBox.Parent()->Parent()->Offset();
  if (mFlags & 1) {
    if (!reader->ReadU64(mBaseDataOffset)) {
      return;
    }
  }
  if (mFlags & 2) {
    if (!reader->ReadU32(mDefaultSampleDescriptionIndex)) {
      return;
    }
  }
  if (mFlags & 8) {
    if (!reader->ReadU32(mDefaultSampleDuration)) {
      return;
    }
  }
  if (mFlags & 0x10) {
    if (!reader->ReadU32(mDefaultSampleSize)) {
      return;
    }
  }
  if (mFlags & 0x20) {
    if (!reader->ReadU32(mDefaultSampleFlags)) {
      return;
    }
  }
  mValid = true;
}

Tfdt::Tfdt(Box& aBox)
{
  BoxReader reader(aBox);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Tfdt, "Incomplete Box (missing flags)");
    return;
  }
  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  uint8_t version = flags >> 24;
  size_t need = version ? sizeof(uint64_t) : sizeof(uint32_t) ;
  if (reader->Remaining() < need) {
    LOG(Tfdt, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  if (version == 0) {
    uint32_t baseDecodeTime;
    if (!reader->ReadU32(baseDecodeTime)) {
      return;
    }
    mBaseMediaDecodeTime = baseDecodeTime;
  } else if (version == 1) {
    if (!reader->ReadU64(mBaseMediaDecodeTime)) {
      return;
    }
  }
  mValid = true;
}

Edts::Edts(Box& aBox)
  : mMediaStart(0)
  , mEmptyOffset(0)
{
  Box child = aBox.FirstChild();
  if (!child.IsType("elst")) {
    return;
  }

  BoxReader reader(child);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Edts, "Incomplete Box (missing flags)");
    return;
  }
  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  uint8_t version = flags >> 24;
  size_t need =
    sizeof(uint32_t) + 2*(version ? sizeof(int64_t) : sizeof(uint32_t));
  if (reader->Remaining() < need) {
    LOG(Edts, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  bool emptyEntry = false;
  uint32_t entryCount;
  if (!reader->ReadU32(entryCount)) {
    return;
  }
  for (uint32_t i = 0; i < entryCount; i++) {
    uint64_t segment_duration;
    int64_t media_time;
    if (version == 1) {
      if (!reader->ReadU64(segment_duration) ||
          !reader->Read64(media_time)) {
        return;
      }
    } else {
      uint32_t duration;
      int32_t time;
      if (!reader->ReadU32(duration) ||
          !reader->Read32(time)) {
        return;
      }
      segment_duration = duration;
      media_time = time;
    }
    if (media_time == -1 && i) {
      LOG(Edts, "Multiple empty edit, not handled");
    } else if (media_time == -1) {
      mEmptyOffset = segment_duration;
      emptyEntry = true;
    } else if (i > 1 || (i > 0 && !emptyEntry)) {
      LOG(Edts, "More than one edit entry, not handled. A/V sync will be wrong");
      break;
    } else {
      mMediaStart = media_time;
    }
    // media_rate_integer and media_rate_fraction
    if (!reader->Skip(4)) {
      return;
    }
  }
  mValid = true;
}

Saiz::Saiz(Box& aBox, AtomType aDefaultType)
  : mAuxInfoType(aDefaultType)
  , mAuxInfoTypeParameter(0)
{
  BoxReader reader(aBox);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Saiz, "Incomplete Box (missing flags)");
    return;
  }
  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  uint8_t version = flags >> 24;
  size_t need =
    ((flags & 1) ? 2*sizeof(uint32_t) : 0) + sizeof(uint8_t) + sizeof(uint32_t);
  if (reader->Remaining() < need) {
    LOG(Saiz, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  if (flags & 1) {
    uint32_t auxType;
    if (!reader->ReadU32(auxType) ||
        !reader->ReadU32(mAuxInfoTypeParameter)) {
      return;
    }
    mAuxInfoType = auxType;
  }
  uint8_t defaultSampleInfoSize;
  uint32_t count;
  if (!reader->ReadU8(defaultSampleInfoSize) ||
      !reader->ReadU32(count)) {
    return;
  }
  if (defaultSampleInfoSize) {
    if (!mSampleInfoSize.SetLength(count, fallible)) {
      LOG(Saiz, "OOM");
      return;
    }
    memset(mSampleInfoSize.Elements(), defaultSampleInfoSize, mSampleInfoSize.Length());
  } else {
    if (!reader->ReadArray(mSampleInfoSize, count)) {
      LOG(Saiz, "Incomplete Box (OOM or missing count:%u)", count);
      return;
    }
  }
  mValid = true;
}

Saio::Saio(Box& aBox, AtomType aDefaultType)
  : mAuxInfoType(aDefaultType)
  , mAuxInfoTypeParameter(0)
{
  BoxReader reader(aBox);
  if (!reader->CanReadType<uint32_t>()) {
    LOG(Saio, "Incomplete Box (missing flags)");
    return;
  }
  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  uint8_t version = flags >> 24;
  size_t need = ((flags & 1) ? (2 * sizeof(uint32_t)) : 0) + sizeof(uint32_t);
  if (reader->Remaining() < need) {
    LOG(Saio, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  if (flags & 1) {
    uint32_t auxType;
    if (!reader->ReadU32(auxType) ||
        !reader->ReadU32(mAuxInfoTypeParameter)) {
      return;
    }
    mAuxInfoType = auxType;
  }
  uint32_t count;
  if (!reader->ReadU32(count)) {
    return;
  }
  need = (version ? sizeof(uint64_t) : sizeof(uint32_t)) * count;
  if (reader->Remaining() < need) {
    LOG(Saio, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  if (!mOffsets.SetCapacity(count, fallible)) {
    LOG(Saiz, "OOM");
    return;
  }
  if (version == 0) {
    uint32_t offset;
    for (size_t i = 0; i < count; i++) {
      if (!reader->ReadU32(offset)) {
        return;
      }
      MOZ_ALWAYS_TRUE(mOffsets.AppendElement(offset, fallible));
    }
  } else {
    uint64_t offset;
    for (size_t i = 0; i < count; i++) {
      if (!reader->ReadU64(offset)) {
        return;
      }
      MOZ_ALWAYS_TRUE(mOffsets.AppendElement(offset, fallible));
    }
  }
  mValid = true;
}

Sbgp::Sbgp(Box& aBox)
{
  BoxReader reader(aBox);

  if (!reader->CanReadType<uint32_t>()) {
    LOG(Sbgp, "Incomplete Box (missing flags)");
    return;
  }

  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  const uint8_t version = flags >> 24;
  flags = flags & 0xffffff;

  // Make sure we have enough bytes to read as far as the count.
  uint32_t need = (version == 1 ? sizeof(uint32_t) : 0) + sizeof(uint32_t) * 2;
  if (reader->Remaining() < need) {
    LOG(Sbgp, "Incomplete Box (have:%lld, need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }

  uint32_t groupingType;
  if (!reader->ReadU32(groupingType)) {
    return;
  }
  mGroupingType = groupingType;

  if (version == 1) {
    if (!reader->ReadU32(mGroupingTypeParam)) {
      return;
    }
  }

  uint32_t count;
  if (!reader->ReadU32(count)) {
    return;
  }

  // Make sure we can read all the entries.
  need = sizeof(uint32_t) * 2 * count;
  if (reader->Remaining() < need) {
    LOG(Sbgp, "Incomplete Box (have:%lld, need:%lld). Failed to read entries",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }

  for (uint32_t i = 0; i < count; i++) {
    uint32_t sampleCount, groupDescriptionIndex;
    if (!reader->ReadU32(sampleCount)) {
      return;
    }
    if (!reader->ReadU32(groupDescriptionIndex)) {
      return;
    }

    SampleToGroupEntry entry(sampleCount, groupDescriptionIndex);
    mEntries.AppendElement(entry);
  }

  mValid = true;
}

Sgpd::Sgpd(Box& aBox)
{
  BoxReader reader(aBox);

  if (!reader->CanReadType<uint32_t>()) {
    LOG(Sgpd, "Incomplete Box (missing flags)");
    return;
  }

  uint32_t flags;
  if (!reader->ReadU32(flags)) {
    return;
  }
  const uint8_t version = flags >> 24;
  flags = flags & 0xffffff;

  uint32_t need = ((flags & 1) ? sizeof(uint32_t) : 0) + sizeof(uint32_t) * 2;
  if (reader->Remaining() < need) {
    LOG(Sgpd, "Incomplete Box (have:%lld need:%lld)",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }

  uint32_t groupingType;
  if (!reader->ReadU32(groupingType)) {
    return;
  }
  mGroupingType = groupingType;

  const uint32_t entrySize = sizeof(uint32_t) + kKeyIdSize;
  uint32_t defaultLength = 0;

  if (version == 1) {
    if (!reader->ReadU32(defaultLength)) {
      return;
    }
    if (defaultLength < entrySize && defaultLength != 0) {
      return;
    }
  }

  uint32_t count;
  if (!reader->ReadU32(count)) {
    return;
  }

  // Make sure we have sufficient remaining bytes to read the entries.
  need =
    count * (sizeof(uint32_t) * (version == 1 && defaultLength == 0 ? 2 : 1) +
             kKeyIdSize * sizeof(uint8_t));
  if (reader->Remaining() < need) {
    LOG(Sgpd, "Incomplete Box (have:%lld need:%lld). Failed to read entries",
        (uint64_t)reader->Remaining(), (uint64_t)need);
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (version == 1 && defaultLength == 0) {
      uint32_t descriptionLength;
      if (!reader->ReadU32(descriptionLength)) {
        return;
      }

      if (descriptionLength < entrySize) {
        return;
      }
    }

    CencSampleEncryptionInfoEntry entry;
    bool valid = entry.Init(reader);
    if (!valid) {
      return;
    }
    mEntries.AppendElement(entry);
  }

  mValid = true;
}

bool CencSampleEncryptionInfoEntry::Init(BoxReader& aReader)
{
  // Skip a reserved byte.
  aReader->Skip(1);

  uint8_t possiblePatternInfo;
  if (!aReader->ReadU8(possiblePatternInfo)) {
    return false;
  }

  uint8_t flag;
  if (!aReader->ReadU8(flag)) {
    return false;
  }

  if (!aReader->ReadU8(mIVSize)) {
    return false;
  }

  // Read the key id.
  for (uint32_t i = 0; i < kKeyIdSize; ++i) {
    uint8_t keyid;
    if (!aReader->ReadU8(keyid)) {
      return false;
    }
    mKeyId.AppendElement(keyid);
  }

  mIsEncrypted = flag != 0;

  if (mIsEncrypted) {
    if (mIVSize != 8 && mIVSize != 16) {
      return false;
    }
  } else if (mIVSize != 0) {
    return false;
  }

  return true;
}

#undef LOG
}
