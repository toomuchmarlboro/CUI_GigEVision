#pragma once
// Radiometric video to HDF5, written on a background thread so disk I/O never stalls acquisition.
//
// File layout (same as the Python tool, readable with h5py / MATLAB h5read / HDFView):
//   /counts            uint16 (N, H, W), chunk (1,H,W), optional shuffle+deflate
//      attr scale_K_per_count
//   /host_time_ns      uint64 (N)   wall clock when the frame arrived
//   /camera_timestamp  uint64 (N)
//   /frame_id          uint64 (N)
//   file attrs: metadata_json, units, scalar metadata fields, frames, dropped_frames,
//               duration_s, mean_fps
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "a70/json.h"
#include "a70/thermal.h"

namespace a70 {

bool hdf5Available();

class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // compression: "gzip" (shuffle + deflate level 1, ~2x smaller) or "none" (fastest)
    std::string start(const std::string& folder, Json metadata, const std::string& prefix, int decimate = 1,
                      const std::string& compression = "gzip", uint64_t maxFrames = 0, int queueFrames = 120);
    bool push(const ThermalFrame& f);  // false once finished / not recording
    void stop();
    bool active() const { return running_.load(); }

    std::string path() const;
    std::string error() const;
    uint64_t written() const { return written_.load(); }
    uint64_t dropped() const { return dropped_.load(); }
    uint64_t bytes() const { return bytes_.load(); }

private:
    void run();

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<ThermalFrame> q_;
    std::thread th_;
    std::atomic<bool> running_{false}, stopReq_{false};
    std::atomic<uint64_t> written_{0}, dropped_{0}, bytes_{0}, seen_{0};
    std::string path_, error_, compression_;
    Json md_;
    int decimate_ = 1;
    size_t queueMax_ = 120;
    uint64_t maxFrames_ = 0;
};

// ---------------------------------------------------------------- reading
struct RecordingInfo {
    uint64_t frames = 0;
    int width = 0, height = 0;
    double scaleK = 0.01;
    std::string metadataJson;
    uint64_t droppedFrames = 0;
    std::vector<int64_t> hostTimeNs;
};

class RecordingReader {
public:
    explicit RecordingReader(const std::string& path);
    ~RecordingReader();
    const RecordingInfo& info() const { return info_; }
    void readFrame(uint64_t index, ThermalFrame& out);

private:
    long long file_ = -1, dset_ = -1;
    RecordingInfo info_;
};

}  // namespace a70
