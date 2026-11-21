#pragma once
#include "runtime/native_stream.hpp"

class MediaTrack;
class ReaProject;

namespace reaweb {
// Only the REAPER main thread owns these objects. Index hints are always checked
// against the GUID before reading a track. No MediaTrack pointer survives a pass.
class TrackControls {
public:
  struct Watch {
    std::vector<std::string> guids;
    std::vector<int> indices;
    std::string fxGuid, takeGuid, chain = "main";
    int fxIndex = -1;
    std::vector<int> parameters;
    unsigned mask = 0;
    uint64_t epoch = 0, revision = 0, samples = 0, unchanged = 0, missed = 0;
    double lastSample = 0, firstSample = 0, lastPublish = 0, durationMs = 0, sampleToPublishMs = 0;
    const char* trigger = "initial";
    std::vector<double> previous;
  };
  enum class Source { controls, fxParameters };
  explicit TrackControls(const Host& host, Source source = Source::controls);
  bool available() const;
  Watch watchFx(const Json& options);
  Watch watch(const Json& options);
  void begin();
  void end() { readings_.clear(); index_.clear(); }
  void reset() { end(); project_ = nullptr; lastSample_ = 0; }
  int sample(Watch&, double rate, StreamHub&, uint64_t handle, uint64_t& sequence,
             uint64_t& published, uint64_t& dropped);
  Json diagnostics() const;
  static constexpr unsigned maxBytes = 40 + 16 * 4 * 8;
private:
  struct FxReading {
    void* owner = nullptr;
    int index = -1, parameterCount = 0;
    std::map<int, double> values;
  };
  struct Reading {
    MediaTrack* track = nullptr;
    int index = -1;
    unsigned mask = 0;
    std::array<double, 4> values{};
    bool master = false;
    struct Chain {
      void* owner = nullptr;
      std::optional<std::vector<int>> indices;
      std::map<std::string, FxReading> effects;
    };
    std::map<std::pair<std::string,std::string>, Chain> chains;
  };
  const Host& host_;
  const Source source_;
  int (*fxCount_)(MediaTrack*) = nullptr;
  int (*recCount_)(MediaTrack*) = nullptr;
  int (*takeCount_)(void*) = nullptr;
  struct ParameterApi {
    int (*count)(void*, int) = nullptr;
    double (*normalized)(void*, int, int) = nullptr;
  } trackParams_, takeParams_;
  bool containerApi_ = false;
public:
  bool inputAvailable() const { return recCount_ && containerApi_; }
  bool monitoringAvailable() const { return inputAvailable() && host_.master_by_guid; }
  bool takeAvailable() const { return host_.take_by_guid && takeCount_ && takeParams_.count && takeParams_.normalized; }
  bool containersAvailable() const { return containerApi_; }
private:
  FxReading& resolveFx(Reading&, Watch&);
  MediaTrack* (*track_)(ReaProject*, int) = nullptr;
  double (*value_)(MediaTrack*, const char*) = nullptr;
  ReaProject* project_ = nullptr;
  uint64_t generation_ = 0, epoch_ = 0, revision_ = 0, listRevision_ = 0, sourceSequence_ = 0;
  uint64_t callbacks_ = 0, snapshots_ = 0, errors_ = 0;
  double now_ = 0, lastSample_ = 0, maxInterval_ = 0;
  int count_ = 0;
  bool queried_ = false, listChanged_ = false;
  int searched_ = 0;
  std::map<std::string, int> index_;
  std::map<std::string, Reading> readings_;
  Reading& resolve(const std::string&, int hint);
};
}
