#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "capture/encoding/clip_session.h"

namespace swing_capture::encoding {
namespace {

std::unique_ptr<mkvparser::Segment> LoadSegment(mkvparser::MkvReader &reader,
                                                const std::string &path) {
  long long position = 0;  // NOLINT(google-runtime-int)
  mkvparser::EBMLHeader header;
  if (header.Parse(&reader, position) < 0 || header.m_docType == nullptr ||
      std::strcmp(header.m_docType, "webm") != 0) {
    throw std::runtime_error("invalid WebM EBML header: " + path);
  }

  mkvparser::Segment *raw_segment = nullptr;
  if (mkvparser::Segment::CreateInstance(&reader, position, raw_segment) != 0 ||
      raw_segment == nullptr) {
    throw std::runtime_error("invalid WebM segment: " + path);
  }
  std::unique_ptr<mkvparser::Segment> segment(raw_segment);
  if (segment->Load() < 0) {
    throw std::runtime_error("could not load WebM segment: " + path);
  }
  return segment;
}

WebmInspection InspectTrack(const mkvparser::Segment &segment) {
  const mkvparser::Tracks *tracks = segment.GetTracks();
  if (tracks == nullptr || tracks->GetTracksCount() != 1U) {
    throw std::runtime_error("clip WebM must contain exactly one track");
  }
  const mkvparser::Track *track = tracks->GetTrackByIndex(0);
  if (track == nullptr || track->GetType() != mkvparser::Track::kVideo ||
      track->GetCodecId() == nullptr) {
    throw std::runtime_error("clip WebM must contain exactly one video track");
  }
  const std::string_view codec_id(track->GetCodecId());
  if (codec_id != "V_VP8" && codec_id != "V_VP9") {
    throw std::runtime_error("clip WebM video track must use VP8 or VP9");
  }
  const auto *video_track = dynamic_cast<const mkvparser::VideoTrack *>(track);
  if (video_track == nullptr || video_track->GetWidth() <= 0 || video_track->GetHeight() <= 0 ||
      std::cmp_greater(video_track->GetWidth(), std::numeric_limits<std::uint32_t>::max()) ||
      std::cmp_greater(video_track->GetHeight(), std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("clip WebM video dimensions are invalid");
  }
  return {
      .codec = codec_id == "V_VP8" ? "vp8" : "vp9",
      .width = static_cast<std::uint32_t>(video_track->GetWidth()),
      .height = static_cast<std::uint32_t>(video_track->GetHeight()),
  };
}

void ObserveBlock(const mkvparser::Block &block, const mkvparser::Cluster &cluster,
                  WebmInspection &inspection) {
  if (block.GetFrameCount() != 1) {
    throw std::runtime_error("clip WebM must contain one frame per video block");
  }
  const long long time_ns = block.GetTime(&cluster);  // NOLINT(google-runtime-int)
  if (time_ns < 0) {
    throw std::runtime_error("clip WebM contains a negative frame timestamp");
  }
  const auto frame_time = std::chrono::nanoseconds(time_ns);
  if (inspection.frame_count > 0U && frame_time <= inspection.last_frame_time) {
    throw std::runtime_error("clip WebM frame timestamps must be strictly increasing");
  }
  if (inspection.frame_count == 0U) {
    inspection.first_frame_time = frame_time;
  }
  inspection.last_frame_time = frame_time;
  ++inspection.frame_count;
  if (block.IsKey()) {
    ++inspection.keyframe_count;
  }
}

void InspectClusters(mkvparser::Segment &segment, WebmInspection &inspection) {
  for (const mkvparser::Cluster *cluster = segment.GetFirst();
       cluster != nullptr && !cluster->EOS(); cluster = segment.GetNext(cluster)) {
    const mkvparser::BlockEntry *entry = nullptr;
    if (cluster->GetFirst(entry) < 0) {
      throw std::runtime_error("could not parse WebM cluster");
    }
    while (entry != nullptr && !entry->EOS()) {
      const mkvparser::Block *block = entry->GetBlock();
      if (block == nullptr) {
        throw std::runtime_error("clip WebM block is missing");
      }
      ObserveBlock(*block, *cluster, inspection);

      const mkvparser::BlockEntry *next = nullptr;
      if (cluster->GetNext(entry, next) < 0) {
        throw std::runtime_error("could not advance through WebM cluster");
      }
      entry = next;
    }
  }
}

}  // namespace

WebmInspection InspectWebm(const std::filesystem::path &path) {
  mkvparser::MkvReader reader;
  const std::string path_string = path.string();
  if (reader.Open(path_string.c_str()) < 0) {
    throw std::runtime_error("could not open WebM for inspection: " + path_string);
  }

  const std::unique_ptr<mkvparser::Segment> segment = LoadSegment(reader, path_string);
  WebmInspection inspection = InspectTrack(*segment);
  InspectClusters(*segment, inspection);
  if (inspection.frame_count == 0U) {
    throw std::runtime_error("clip WebM contains no video frames");
  }
  return inspection;
}

WebmInspection InspectVp8Webm(const std::filesystem::path &path) {
  WebmInspection inspection = InspectWebm(path);
  if (inspection.codec != "vp8") {
    throw std::runtime_error("clip WebM must contain a VP8 video track");
  }
  return inspection;
}

}  // namespace swing_capture::encoding
