// GigE Vision through Aravis 0.8 (LGPL, Linux incl. Jetson / Raspberry Pi).
//   Debian/Ubuntu/Pi OS: sudo apt install libaravis-dev aravis-tools
#include "a70/backend.h"

#ifdef A70_HAVE_ARAVIS
#include <arv.h>

#include <algorithm>
#include <cstring>

namespace a70 {
namespace {

std::string takeError(GError*& e) {
    std::string s = e ? e->message : "unknown error";
    if (e) g_clear_error(&e);
    return s;
}

struct Str {  // safe const char* -> std::string
    static std::string of(const char* s) { return s ? s : ""; }
};

class AravisBackend : public Backend {
public:
    ~AravisBackend() override { close(); }
    std::string name() const override { return "Aravis (GigE Vision, Linux)"; }

    std::vector<DeviceInfo> listDevices() override {
        arv_update_device_list();
        std::vector<DeviceInfo> v;
        unsigned n = arv_get_n_devices();
        for (unsigned i = 0; i < n; ++i)
            v.push_back({Str::of(arv_get_device_id(i)), Str::of(arv_get_device_model(i)),
                         Str::of(arv_get_device_vendor(i)), Str::of(arv_get_device_serial_nbr(i)),
                         Str::of(arv_get_device_address(i))});
        return v;
    }

    DeviceInfo open(const std::string& id) override {
        close();
        GError* e = nullptr;
        cam_ = arv_camera_new(id.empty() ? nullptr : id.c_str(), &e);
        if (!cam_) throw CameraError("Could not open camera '" + id + "': " + takeError(e));
        dev_ = arv_camera_get_device(cam_);  // owned by camera
        DeviceInfo d;
        d.id = id;
        d.model = Str::of(arv_camera_get_model_name(cam_, nullptr));
        d.vendor = Str::of(arv_camera_get_vendor_name(cam_, nullptr));
        if (has("DeviceSerialNumber")) d.serial = getString("DeviceSerialNumber");
        return d;
    }

    void close() override {
        stop();
        if (cam_) g_object_unref(cam_);
        cam_ = nullptr;
        dev_ = nullptr;
    }
    bool isOpen() const override { return cam_ != nullptr; }

    ArvGcNode* node(const std::string& n) {
        if (!dev_) throw CameraError("Camera not open");
        return arv_device_get_feature(dev_, n.c_str());
    }

    bool has(const std::string& n) override {
        ArvGcNode* nd = node(n);
        if (!nd) return false;
        if (!ARV_IS_GC_FEATURE_NODE(nd)) return true;
        GError* e = nullptr;
        bool ok = arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(nd), &e) &&
                  arv_gc_feature_node_is_available(ARV_GC_FEATURE_NODE(nd), &e);
        if (e) g_clear_error(&e);
        return ok;
    }

    static FeatureType kindOf(ArvGcNode* nd) {
        if (!nd) return FeatureType::Other;
        if (ARV_IS_GC_COMMAND(nd)) return FeatureType::Command;
        if (ARV_IS_GC_ENUMERATION(nd)) return FeatureType::Enumeration;
        if (ARV_IS_GC_BOOLEAN(nd)) return FeatureType::Boolean;
        if (ARV_IS_GC_CATEGORY(nd)) return FeatureType::Category;
        if (ARV_IS_GC_FLOAT(nd)) return FeatureType::Float;
        if (ARV_IS_GC_INTEGER(nd)) return FeatureType::Integer;
        if (ARV_IS_GC_STRING(nd)) return FeatureType::String;
        if (ARV_IS_GC_REGISTER(nd)) return FeatureType::Register;
        return FeatureType::Other;
    }

    FeatureType type(const std::string& n) override { return kindOf(node(n)); }

    std::string getString(const std::string& n) override {
        ArvGcNode* nd = node(n);
        if (!nd) throw CameraError("Feature " + n + " not found");
        GError* e = nullptr;
        const char* s = arv_gc_feature_node_get_value_as_string(ARV_GC_FEATURE_NODE(nd), &e);
        if (e) throw CameraError("Read " + n + ": " + takeError(e));
        return Str::of(s);
    }

    double getFloat(const std::string& n) override {
        ArvGcNode* nd = node(n);
        if (!nd) throw CameraError("Feature " + n + " not found");
        GError* e = nullptr;
        double v = ARV_IS_GC_FLOAT(nd) ? arv_device_get_float_feature_value(dev_, n.c_str(), &e)
                                       : double(arv_device_get_integer_feature_value(dev_, n.c_str(), &e));
        if (e) throw CameraError("Read " + n + ": " + takeError(e));
        return v;
    }

    int64_t getInt(const std::string& n) override {
        ArvGcNode* nd = node(n);
        if (!nd) throw CameraError("Feature " + n + " not found");
        GError* e = nullptr;
        int64_t v = ARV_IS_GC_INTEGER(nd) ? arv_device_get_integer_feature_value(dev_, n.c_str(), &e)
                                          : int64_t(arv_device_get_float_feature_value(dev_, n.c_str(), &e));
        if (e) throw CameraError("Read " + n + ": " + takeError(e));
        return v;
    }

    void setString(const std::string& n, const std::string& v) override {
        ArvGcNode* nd = node(n);
        if (!nd) throw CameraError("Feature " + n + " not found");
        GError* e = nullptr;
        FeatureType k = kindOf(nd);
        if (k == FeatureType::Float) {
            arv_device_set_float_feature_value(dev_, n.c_str(), std::stod(v), &e);
        } else if (k == FeatureType::Integer) {
            arv_device_set_integer_feature_value(dev_, n.c_str(), std::stoll(v), &e);
        } else if (k == FeatureType::Boolean) {
            bool b = v == "1" || v == "true" || v == "True" || v == "on";
            arv_device_set_boolean_feature_value(dev_, n.c_str(), b, &e);
        } else if (k == FeatureType::Enumeration || k == FeatureType::String) {
            arv_device_set_string_feature_value(dev_, n.c_str(), v.c_str(), &e);
        } else {
            arv_gc_feature_node_set_value_from_string(ARV_GC_FEATURE_NODE(nd), v.c_str(), &e);
        }
        if (e) throw CameraError("Write " + n + "=" + v + ": " + takeError(e));
    }

    void setFloat(const std::string& n, double v) override {
        if (kindOf(node(n)) != FeatureType::Float) return Backend::setFloat(n, v);
        GError* e = nullptr;
        arv_device_set_float_feature_value(dev_, n.c_str(), v, &e);
        if (e) throw CameraError("Write " + n + ": " + takeError(e));
    }

    void execute(const std::string& n) override {
        GError* e = nullptr;
        arv_device_execute_command(dev_, n.c_str(), &e);
        if (e) throw CameraError("Execute " + n + ": " + takeError(e));
    }

    std::vector<std::string> choices(const std::string& n) override {
        std::vector<std::string> v;
        GError* e = nullptr;
        guint cnt = 0;
        const char** vals = arv_device_dup_available_enumeration_feature_values_as_strings(dev_, n.c_str(), &cnt, &e);
        if (e) {
            g_clear_error(&e);
            return v;
        }
        for (guint i = 0; i < cnt; ++i) v.push_back(Str::of(vals[i]));
        g_free(vals);
        return v;
    }

    std::vector<Feature> features() override {
        std::vector<Feature> out;
        if (!dev_) return out;
        ArvGc* gc = arv_device_get_genicam(dev_);
        ArvGcNode* root = arv_gc_get_node(gc, "Root");
        std::vector<std::string> seen;
        walk(root, "Root", out, seen);
        return out;
    }

    void walk(ArvGcNode* cat, const std::string& catName, std::vector<Feature>& out, std::vector<std::string>& seen) {
        if (!cat || !ARV_IS_GC_CATEGORY(cat)) return;
        for (const GSList* it = arv_gc_category_get_features(ARV_GC_CATEGORY(cat)); it; it = it->next) {
            std::string fname = static_cast<const char*>(it->data);
            if (std::find(seen.begin(), seen.end(), fname) != seen.end()) continue;
            seen.push_back(fname);
            ArvGcNode* nd = node(fname);
            if (!nd) continue;
            FeatureType k = kindOf(nd);
            if (k == FeatureType::Category) {
                walk(nd, fname, out, seen);
                continue;
            }
            if (!ARV_IS_GC_FEATURE_NODE(nd) || !has(fname)) continue;
            Feature f;
            f.name = fname;
            f.category = catName;
            f.type = k;
            ArvGcAccessMode mode = arv_gc_feature_node_get_actual_access_mode(ARV_GC_FEATURE_NODE(nd));
            f.readable = mode == ARV_GC_ACCESS_MODE_RO || mode == ARV_GC_ACCESS_MODE_RW;
            f.writable = mode == ARV_GC_ACCESS_MODE_WO || mode == ARV_GC_ACCESS_MODE_RW || k == FeatureType::Command;
            if (f.readable && k != FeatureType::Command) {
                try {
                    f.value = getString(fname);
                } catch (const std::exception& ex) {
                    f.value = std::string("<") + ex.what() + ">";
                }
            }
            if (k == FeatureType::Enumeration) f.choices = choices(fname);
            f.description = Str::of(arv_gc_feature_node_get_description(ARV_GC_FEATURE_NODE(nd)));
            if (k == FeatureType::Float) f.unit = Str::of(arv_gc_float_get_unit(ARV_GC_FLOAT(nd)));
            out.push_back(std::move(f));
        }
    }

    void configureStream() override {
        if (!cam_) return;
        GError* e = nullptr;
        if (arv_camera_is_gv_device(cam_)) {
            // largest packet the NIC/switch path supports (8k+ jumbo, ~1.4k standard e.g. Pi)
            arv_camera_gv_auto_packet_size(cam_, &e);
            if (e) g_clear_error(&e);
        }
    }

    void start() override {
        if (stream_) return;
        GError* e = nullptr;
        arv_camera_set_acquisition_mode(cam_, ARV_ACQUISITION_MODE_CONTINUOUS, &e);
        if (e) g_clear_error(&e);
        stream_ = arv_camera_create_stream(cam_, nullptr, nullptr, &e);
        if (!stream_) throw CameraError("Create stream: " + takeError(e));
        guint payload = arv_camera_get_payload(cam_, &e);
        if (e) throw CameraError("Payload: " + takeError(e));
        for (int i = 0; i < 16; ++i) arv_stream_push_buffer(stream_, arv_buffer_new_allocate(payload));
        arv_camera_start_acquisition(cam_, &e);
        if (e) {
            g_object_unref(stream_);
            stream_ = nullptr;
            throw CameraError("Start acquisition: " + takeError(e));
        }
    }

    void stop() override {
        if (!stream_) return;
        GError* e = nullptr;
        if (cam_) arv_camera_stop_acquisition(cam_, &e);
        if (e) g_clear_error(&e);
        g_object_unref(stream_);
        stream_ = nullptr;
    }

    bool grab(RawFrame& out, double timeoutS) override {
        if (!stream_) throw CameraError("Not streaming");
        ArvBuffer* buf = arv_stream_timeout_pop_buffer(stream_, static_cast<guint64>(timeoutS * 1e6));
        if (!buf) return false;
        bool ok = arv_buffer_get_status(buf) == ARV_BUFFER_STATUS_SUCCESS;
        if (ok) {
            size_t size = 0;
            const void* data = arv_buffer_get_data(buf, &size);
            int w = arv_buffer_get_image_width(buf), h = arv_buffer_get_image_height(buf);
            size_t need = size_t(w) * h * 2;
            if (!data || size < need || w <= 0 || h <= 0) {
                ok = false;
            } else {
                out.width = w;
                out.height = h;
                out.data.resize(size_t(w) * h);
                std::memcpy(out.data.data(), data, need);  // GigE Vision Mono16 is little-endian
                out.hostTimeNs = nowNs();
                out.cameraTimestamp = arv_buffer_get_timestamp(buf);
                out.frameId = arv_buffer_get_frame_id(buf);
            }
        }
        ok ? ++ok_ : ++fail_;
        arv_stream_push_buffer(stream_, buf);
        return ok;
    }

    std::string stats() override {
        std::string s = "ok " + std::to_string(ok_) + ", failed " + std::to_string(fail_);
        if (stream_) {
            guint64 c = 0, f = 0, u = 0;
            arv_stream_get_statistics(stream_, &c, &f, &u);
            s += ", completed " + std::to_string(c) + ", failures " + std::to_string(f) + ", underruns " +
                 std::to_string(u);
        }
        return s;
    }

private:
    ArvCamera* cam_ = nullptr;
    ArvDevice* dev_ = nullptr;
    ArvStream* stream_ = nullptr;
    uint64_t ok_ = 0, fail_ = 0;
};

}  // namespace

std::unique_ptr<Backend> makeAravisBackend() { return std::make_unique<AravisBackend>(); }

}  // namespace a70

#else
namespace a70 {
std::unique_ptr<Backend> makeAravisBackend() { return nullptr; }
}  // namespace a70
#endif
