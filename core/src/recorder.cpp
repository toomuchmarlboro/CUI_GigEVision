#include "a70/recorder.h"

#include "a70/io.h"

#ifdef A70_HAVE_HDF5
#include <hdf5.h>
#endif

#include <chrono>
#include <stdexcept>

namespace a70 {

#ifdef A70_HAVE_HDF5
bool hdf5Available() { return true; }

namespace {
std::mutex& h5mutex() {  // the stock HDF5 build is not thread-safe: serialise every call
    static std::mutex m;
    return m;
}

struct H5Init {
    H5Init() { H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr); }  // we check return codes ourselves
};
void h5init() { static H5Init once; }

void check(herr_t r, const char* what) {
    if (r < 0) throw std::runtime_error(std::string("HDF5: ") + what + " failed");
}
hid_t checkId(hid_t id, const char* what) {
    if (id < 0) throw std::runtime_error(std::string("HDF5: ") + what + " failed");
    return id;
}

void delAttr(hid_t obj, const char* name) {
    if (H5Aexists(obj, name) > 0) H5Adelete(obj, name);
}

void attrDouble(hid_t obj, const char* name, double v) {
    delAttr(obj, name);
    hid_t sp = H5Screate(H5S_SCALAR);
    hid_t a = checkId(H5Acreate2(obj, name, H5T_IEEE_F64LE, sp, H5P_DEFAULT, H5P_DEFAULT), name);
    H5Awrite(a, H5T_NATIVE_DOUBLE, &v);
    H5Aclose(a);
    H5Sclose(sp);
}

void attrInt(hid_t obj, const char* name, long long v) {
    delAttr(obj, name);
    hid_t sp = H5Screate(H5S_SCALAR);
    hid_t a = checkId(H5Acreate2(obj, name, H5T_STD_I64LE, sp, H5P_DEFAULT, H5P_DEFAULT), name);
    H5Awrite(a, H5T_NATIVE_LLONG, &v);
    H5Aclose(a);
    H5Sclose(sp);
}

void attrString(hid_t obj, const char* name, const std::string& v) {
    delAttr(obj, name);
    hid_t t = H5Tcopy(H5T_C_S1);
    H5Tset_size(t, H5T_VARIABLE);
    H5Tset_cset(t, H5T_CSET_UTF8);
    hid_t sp = H5Screate(H5S_SCALAR);
    hid_t a = checkId(H5Acreate2(obj, name, t, sp, H5P_DEFAULT, H5P_DEFAULT), name);
    const char* p = v.c_str();
    H5Awrite(a, t, &p);
    H5Aclose(a);
    H5Sclose(sp);
    H5Tclose(t);
}

bool readAttrString(hid_t obj, const char* name, std::string& out) {
    if (H5Aexists(obj, name) <= 0) return false;
    hid_t a = H5Aopen(obj, name, H5P_DEFAULT);
    hid_t ft = H5Aget_type(a);
    bool ok = false;
    if (H5Tget_class(ft) == H5T_STRING) {
        if (H5Tis_variable_str(ft) > 0) {
            hid_t mt = H5Tcopy(H5T_C_S1);
            H5Tset_size(mt, H5T_VARIABLE);
            H5Tset_cset(mt, H5Tget_cset(ft));
            char* p = nullptr;
            if (H5Aread(a, mt, &p) >= 0 && p) {
                out = p;
                hid_t sp = H5Aget_space(a);
                H5Dvlen_reclaim(mt, sp, H5P_DEFAULT, &p);
                H5Sclose(sp);
                ok = true;
            }
            H5Tclose(mt);
        } else {
            size_t n = H5Tget_size(ft);
            std::string s(n, '\0');
            if (H5Aread(a, ft, s.data()) >= 0) {
                out = s.c_str();
                ok = true;
            }
        }
    }
    H5Tclose(ft);
    H5Aclose(a);
    return ok;
}

bool readAttrDouble(hid_t obj, const char* name, double& v) {
    if (H5Aexists(obj, name) <= 0) return false;
    hid_t a = H5Aopen(obj, name, H5P_DEFAULT);
    bool ok = H5Aread(a, H5T_NATIVE_DOUBLE, &v) >= 0;
    H5Aclose(a);
    return ok;
}

struct H5Writer {
    hid_t file = -1, counts = -1, ts = -1, cts = -1, fid = -1;
    hsize_t n = 0;
    int w = 0, h = 0;

    hid_t make1d(const char* name, hid_t type) {
        hsize_t d[1] = {0}, mx[1] = {H5S_UNLIMITED}, ch[1] = {1024};
        hid_t sp = H5Screate_simple(1, d, mx);
        hid_t pl = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_chunk(pl, 1, ch);
        hid_t ds = checkId(H5Dcreate2(file, name, type, sp, H5P_DEFAULT, pl, H5P_DEFAULT), name);
        H5Pclose(pl);
        H5Sclose(sp);
        return ds;
    }

    void open(const std::string& path, int width, int height, bool gzip) {
        w = width;
        h = height;
        file = checkId(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT), "create file");
        hsize_t d[3] = {0, hsize_t(h), hsize_t(w)}, mx[3] = {H5S_UNLIMITED, hsize_t(h), hsize_t(w)},
                ch[3] = {1, hsize_t(h), hsize_t(w)};
        hid_t sp = H5Screate_simple(3, d, mx);
        hid_t pl = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_chunk(pl, 3, ch);
        if (gzip) {
            H5Pset_shuffle(pl);
            H5Pset_deflate(pl, 1);
        }
        counts = checkId(H5Dcreate2(file, "counts", H5T_STD_U16LE, sp, H5P_DEFAULT, pl, H5P_DEFAULT), "counts");
        H5Pclose(pl);
        H5Sclose(sp);
        ts = make1d("host_time_ns", H5T_STD_U64LE);
        cts = make1d("camera_timestamp", H5T_STD_U64LE);
        fid = make1d("frame_id", H5T_STD_U64LE);
    }

    void append1d(hid_t ds, uint64_t v) {
        hsize_t nd[1] = {n + 1}, st[1] = {n}, c[1] = {1};
        check(H5Dset_extent(ds, nd), "extend");
        hid_t fs = H5Dget_space(ds);
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, st, nullptr, c, nullptr);
        hid_t ms = H5Screate_simple(1, c, nullptr);
        herr_t r = H5Dwrite(ds, H5T_NATIVE_UINT64, ms, fs, H5P_DEFAULT, &v);
        H5Sclose(ms);
        H5Sclose(fs);
        check(r, "write");
    }

    void append(const ThermalFrame& f) {
        if (f.width != w || f.height != h) throw std::runtime_error("frame size changed during recording");
        hsize_t nd[3] = {n + 1, hsize_t(h), hsize_t(w)}, st[3] = {n, 0, 0}, c[3] = {1, hsize_t(h), hsize_t(w)};
        check(H5Dset_extent(counts, nd), "extend counts");
        hid_t fs = H5Dget_space(counts);
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, st, nullptr, c, nullptr);
        hid_t ms = H5Screate_simple(3, c, nullptr);
        herr_t r = H5Dwrite(counts, H5T_NATIVE_UINT16, ms, fs, H5P_DEFAULT, f.counts.data());
        H5Sclose(ms);
        H5Sclose(fs);
        check(r, "write counts (disk full?)");
        append1d(ts, uint64_t(f.hostTimeNs));
        append1d(cts, f.cameraTimestamp);
        append1d(fid, f.frameId);
        ++n;
    }

    void close() {
        for (hid_t* d : {&counts, &ts, &cts, &fid})
            if (*d >= 0) { H5Dclose(*d); *d = -1; }
        if (file >= 0) { H5Fclose(file); file = -1; }
    }
};
}  // namespace

Recorder::Recorder() { h5init(); }
Recorder::~Recorder() { stop(); }

std::string Recorder::start(const std::string& folder, Json md, const std::string& prefix, int decimate,
                            const std::string& compression, uint64_t maxFrames, int queueFrames) {
    if (running_) throw std::runtime_error("Already recording");
    if (th_.joinable()) th_.join();
    makeDirs(folder);
    {
        std::lock_guard<std::mutex> lk(m_);
        path_ = joinPath(folder, timestampStem(prefix) + ".h5");
        error_.clear();
        q_.clear();
        compression_ = compression;
        md_ = std::move(md);
        decimate_ = std::max(1, decimate);
        maxFrames_ = maxFrames;
        queueMax_ = size_t(std::max(4, queueFrames));
    }
    written_ = dropped_ = bytes_ = seen_ = 0;
    stopReq_ = false;
    running_ = true;
    th_ = std::thread(&Recorder::run, this);
    return path_;
}

bool Recorder::push(const ThermalFrame& f) {
    if (!running_ || stopReq_) return false;
    uint64_t s = seen_++;
    if (s % uint64_t(decimate_)) return true;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= queueMax_) {
            ++dropped_;
            return true;
        }
        q_.push_back(f);
    }
    cv_.notify_one();
    return true;
}

void Recorder::stop() {
    stopReq_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    running_ = false;
}

std::string Recorder::path() const {
    std::lock_guard<std::mutex> lk(m_);
    return path_;
}
std::string Recorder::error() const {
    std::lock_guard<std::mutex> lk(m_);
    return error_;
}

void Recorder::run() {
    H5Writer w;
    std::string path;
    Json md;
    {
        std::lock_guard<std::mutex> lk(m_);
        path = path_;
        md = md_;
    }
    int64_t firstTs = 0, lastTs = 0;
    try {
        int W = int(md.find("width") ? md.find("width")->number() : 640);
        int H = int(md.find("height") ? md.find("height")->number() : 480);
        double scale = md.find("scale_K_per_count") ? md.find("scale_K_per_count")->number() : 0.01;
        {
            std::lock_guard<std::mutex> hl(h5mutex());
            w.open(path, W, H, compression_ == "gzip");
            for (auto& kv : md.items()) {
                const Json& v = kv.second;
                if (v.kind() == Json::Kind::String) attrString(w.file, kv.first.c_str(), v.str());
                else if (v.kind() == Json::Kind::Int) attrInt(w.file, kv.first.c_str(), (long long)v.number());
                else if (v.kind() == Json::Kind::Number) attrDouble(w.file, kv.first.c_str(), v.number());
            }
            attrString(w.file, "metadata_json", md.dump());
            attrString(w.file, "units", "counts; T_degC = counts * scale_K_per_count - 273.15");
            attrString(w.file, "compression", compression_);
            attrDouble(w.counts, "scale_K_per_count", scale);
        }
        auto lastFlush = std::chrono::steady_clock::now();
        while (true) {
            ThermalFrame f;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait_for(lk, std::chrono::milliseconds(200), [&] { return !q_.empty() || stopReq_; });
                if (q_.empty()) {
                    if (stopReq_) break;
                    continue;
                }
                f = std::move(q_.front());
                q_.pop_front();
            }
            {
                std::lock_guard<std::mutex> hl(h5mutex());
                w.append(f);
                if (std::chrono::steady_clock::now() - lastFlush > std::chrono::seconds(1)) {
                    H5Fflush(w.file, H5F_SCOPE_LOCAL);  // limit data loss on power failure
                    lastFlush = std::chrono::steady_clock::now();
                }
            }
            if (!firstTs) firstTs = f.hostTimeNs;
            lastTs = f.hostTimeNs;
            ++written_;
            bytes_ += f.counts.size() * 2;
            if (maxFrames_ && written_ >= maxFrames_) {
                stopReq_ = true;
                break;
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(m_);
        error_ = e.what();
        stopReq_ = true;
    }
    {
        std::lock_guard<std::mutex> hl(h5mutex());
        if (w.file >= 0) {
            try {
                attrInt(w.file, "frames", (long long)written_.load());
                attrInt(w.file, "dropped_frames", (long long)dropped_.load());
                if (written_ > 1 && lastTs > firstTs) {
                    double dur = (lastTs - firstTs) / 1e9;
                    attrDouble(w.file, "duration_s", dur);
                    attrDouble(w.file, "mean_fps", (written_ - 1) / dur);
                }
            } catch (...) {
            }
        }
        w.close();
    }
    running_ = false;
}

// ---------------------------------------------------------------- reader
RecordingReader::RecordingReader(const std::string& path) {
    h5init();
    std::lock_guard<std::mutex> hl(h5mutex());
    hid_t f = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (f < 0) throw std::runtime_error("Cannot open HDF5 file " + path);
    file_ = f;
    hid_t d = H5Dopen2(f, "counts", H5P_DEFAULT);
    if (d < 0) throw std::runtime_error("No /counts dataset in " + path);
    dset_ = d;
    hid_t sp = H5Dget_space(d);
    hsize_t dims[3] = {0, 0, 0};
    H5Sget_simple_extent_dims(sp, dims, nullptr);
    H5Sclose(sp);
    info_.frames = dims[0];
    info_.height = int(dims[1]);
    info_.width = int(dims[2]);
    readAttrDouble(d, "scale_K_per_count", info_.scaleK);
    readAttrString(f, "metadata_json", info_.metadataJson);
    double dr = 0;
    if (readAttrDouble(f, "dropped_frames", dr)) info_.droppedFrames = uint64_t(dr);
    hid_t t = H5Dopen2(f, "host_time_ns", H5P_DEFAULT);
    if (t >= 0) {
        info_.hostTimeNs.resize(info_.frames);
        if (info_.frames) H5Dread(t, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, info_.hostTimeNs.data());
        H5Dclose(t);
    }
}

RecordingReader::~RecordingReader() {
    std::lock_guard<std::mutex> hl(h5mutex());
    if (dset_ >= 0) H5Dclose(hid_t(dset_));
    if (file_ >= 0) H5Fclose(hid_t(file_));
}

void RecordingReader::readFrame(uint64_t i, ThermalFrame& out) {
    if (i >= info_.frames) throw std::out_of_range("frame index");
    std::lock_guard<std::mutex> hl(h5mutex());
    out.width = info_.width;
    out.height = info_.height;
    out.scaleK = info_.scaleK;
    out.counts.resize(size_t(info_.width) * info_.height);
    hsize_t st[3] = {i, 0, 0}, c[3] = {1, hsize_t(info_.height), hsize_t(info_.width)};
    hid_t fs = H5Dget_space(hid_t(dset_));
    H5Sselect_hyperslab(fs, H5S_SELECT_SET, st, nullptr, c, nullptr);
    hid_t ms = H5Screate_simple(3, c, nullptr);
    herr_t r = H5Dread(hid_t(dset_), H5T_NATIVE_UINT16, ms, fs, H5P_DEFAULT, out.counts.data());
    H5Sclose(ms);
    H5Sclose(fs);
    if (r < 0) throw std::runtime_error("HDF5 read failed");
    out.hostTimeNs = i < info_.hostTimeNs.size() ? info_.hostTimeNs[i] : 0;
    out.frameId = i;
}

#else  // ---------------------------------------------------------------- no HDF5

bool hdf5Available() { return false; }
Recorder::Recorder() = default;
Recorder::~Recorder() = default;
std::string Recorder::start(const std::string&, Json, const std::string&, int, const std::string&, uint64_t, int) {
    throw std::runtime_error("This build has no HDF5 support — rebuild with libhdf5 to record video");
}
bool Recorder::push(const ThermalFrame&) { return false; }
void Recorder::stop() {}
std::string Recorder::path() const { return {}; }
std::string Recorder::error() const { return {}; }
void Recorder::run() {}
RecordingReader::RecordingReader(const std::string&) { throw std::runtime_error("No HDF5 support in this build"); }
RecordingReader::~RecordingReader() = default;
void RecordingReader::readFrame(uint64_t, ThermalFrame&) {}

#endif

}  // namespace a70
