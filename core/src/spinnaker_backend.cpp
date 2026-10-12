// GigE Vision through the FLIR/Teledyne Spinnaker C++ SDK (Windows, Linux x86-64, Linux ARM64).
// Spinnaker includes the GigE Vision filter driver on Windows and is what MathWorks uses for the A70.
// NOTE: written against the Spinnaker 3.x/4.x API; not compiled in the reference build environment
// (SDK is not redistributable) — build with -DSPINNAKER_DIR=... on a machine that has it.
#include "a70/backend.h"

#ifdef A70_HAVE_SPINNAKER
#include <Spinnaker.h>
#include <SpinGenApi/SpinnakerGenApi.h>

#include <algorithm>
#include <cstring>

namespace a70 {
namespace {

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

std::string tlString(INodeMap& tl, const char* name) {
    try {
        CStringPtr p = tl.GetNode(name);
        if (IsAvailable(p) && IsReadable(p)) return p->GetValue().c_str();
    } catch (...) {
    }
    return {};
}

std::string tlIp(INodeMap& tl) {
    try {
        CIntegerPtr p = tl.GetNode("GevDeviceIPAddress");
        if (IsAvailable(p) && IsReadable(p)) {
            auto ip = static_cast<uint32_t>(p->GetValue());
            return std::to_string((ip >> 24) & 255) + "." + std::to_string((ip >> 16) & 255) + "." +
                   std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
        }
    } catch (...) {
    }
    return {};
}

class SpinnakerBackend : public Backend {
public:
    SpinnakerBackend() { sys_ = System::GetInstance(); }
    ~SpinnakerBackend() override {
        close();
        list_.Clear();
        if (sys_) sys_->ReleaseInstance();
    }
    std::string name() const override { return "Spinnaker SDK (GigE Vision)"; }

    std::vector<DeviceInfo> listDevices() override {
        std::vector<DeviceInfo> v;
        try {
            list_ = sys_->GetCameras();
            for (unsigned i = 0; i < list_.GetSize(); ++i) {
                CameraPtr c = list_.GetByIndex(i);
                INodeMap& tl = c->GetTLDeviceNodeMap();
                DeviceInfo d;
                d.serial = tlString(tl, "DeviceSerialNumber");
                d.id = d.serial.empty() ? std::to_string(i) : d.serial;
                d.model = tlString(tl, "DeviceModelName");
                d.vendor = tlString(tl, "DeviceVendorName");
                d.address = tlIp(tl);
                v.push_back(d);
            }
        } catch (const Spinnaker::Exception& e) {
            throw CameraError(std::string("Spinnaker discovery: ") + e.what());
        }
        return v;
    }

    DeviceInfo open(const std::string& id) override {
        close();
        try {
            list_ = sys_->GetCameras();
            if (list_.GetSize() == 0) throw CameraError("No GigE Vision cameras visible to Spinnaker");
            if (id.empty()) {
                cam_ = list_.GetByIndex(0);
            } else {
                cam_ = list_.GetBySerial(id);
                if (!cam_ && std::all_of(id.begin(), id.end(), ::isdigit) && std::stoul(id) < list_.GetSize())
                    cam_ = list_.GetByIndex(static_cast<unsigned>(std::stoul(id)));
                if (!cam_) {  // match by IP address
                    for (unsigned i = 0; i < list_.GetSize() && !cam_; ++i)
                        if (tlIp(list_.GetByIndex(i)->GetTLDeviceNodeMap()) == id) cam_ = list_.GetByIndex(i);
                }
            }
            if (!cam_) throw CameraError("Camera '" + id + "' not found");
            INodeMap& tl = cam_->GetTLDeviceNodeMap();
            DeviceInfo d{id, tlString(tl, "DeviceModelName"), tlString(tl, "DeviceVendorName"),
                         tlString(tl, "DeviceSerialNumber"), tlIp(tl)};
            cam_->Init();
            nm_ = &cam_->GetNodeMap();
            // keep every frame (recording) with a reasonable buffer pool
            try {
                INodeMap& s = cam_->GetTLStreamNodeMap();
                CEnumerationPtr mode = s.GetNode("StreamBufferCountMode");
                if (IsAvailable(mode) && IsWritable(mode)) mode->FromString("Manual");
                CIntegerPtr cnt = s.GetNode("StreamBufferCountManual");
                if (IsAvailable(cnt) && IsWritable(cnt)) cnt->SetValue(std::min<int64_t>(16, cnt->GetMax()));
                CEnumerationPtr h = s.GetNode("StreamBufferHandlingMode");
                if (IsAvailable(h) && IsWritable(h)) h->FromString("OldestFirst");
            } catch (...) {
            }
            return d;
        } catch (const Spinnaker::Exception& e) {
            cam_ = nullptr;
            throw CameraError(std::string("Spinnaker open: ") + e.what());
        }
    }

    void close() override {
        stop();
        if (cam_) {
            try {
                if (cam_->IsInitialized()) cam_->DeInit();
            } catch (...) {
            }
        }
        cam_ = nullptr;
        nm_ = nullptr;
    }
    bool isOpen() const override { return cam_ && nm_; }

    CNodePtr node(const std::string& n) {
        if (!nm_) throw CameraError("Camera not open");
        return nm_->GetNode(gcstring(n.c_str()));
    }

    bool has(const std::string& n) override {
        try {
            CNodePtr p = node(n);
            return p && IsImplemented(p) && IsAvailable(p);
        } catch (const Spinnaker::Exception&) {
            return false;
        }
    }

    static FeatureType kindOf(CNodePtr p) {
        if (!p) return FeatureType::Other;
        switch (p->GetPrincipalInterfaceType()) {
            case intfIInteger: return FeatureType::Integer;
            case intfIFloat: return FeatureType::Float;
            case intfIBoolean: return FeatureType::Boolean;
            case intfIString: return FeatureType::String;
            case intfIEnumeration: return FeatureType::Enumeration;
            case intfICommand: return FeatureType::Command;
            case intfICategory: return FeatureType::Category;
            case intfIRegister: return FeatureType::Register;
            default: return FeatureType::Other;
        }
    }
    FeatureType type(const std::string& n) override { return kindOf(node(n)); }

    std::string getString(const std::string& n) override {
        try {
            CValuePtr v = node(n);
            if (!v) throw CameraError("Feature " + n + " not found");
            return v->ToString().c_str();
        } catch (const Spinnaker::Exception& e) {
            throw CameraError("Read " + n + ": " + e.what());
        }
    }

    double getFloat(const std::string& n) override {
        try {
            CNodePtr p = node(n);
            if (kindOf(p) == FeatureType::Float) return CFloatPtr(p)->GetValue();
            if (kindOf(p) == FeatureType::Integer) return double(CIntegerPtr(p)->GetValue());
        } catch (const Spinnaker::Exception& e) {
            throw CameraError("Read " + n + ": " + e.what());
        }
        return Backend::getFloat(n);
    }

    void setString(const std::string& n, const std::string& v) override {
        try {
            CValuePtr p = node(n);
            if (!p) throw CameraError("Feature " + n + " not found");
            p->FromString(gcstring(v.c_str()));
        } catch (const Spinnaker::Exception& e) {
            throw CameraError("Write " + n + "=" + v + ": " + e.what());
        }
    }

    void setFloat(const std::string& n, double v) override {
        try {
            CNodePtr p = node(n);
            if (kindOf(p) == FeatureType::Float) {
                CFloatPtr(p)->SetValue(v);
                return;
            }
        } catch (const Spinnaker::Exception& e) {
            throw CameraError("Write " + n + ": " + e.what());
        }
        Backend::setFloat(n, v);
    }

    void execute(const std::string& n) override {
        try {
            CCommandPtr c = node(n);
            if (!c) throw CameraError("Command " + n + " not found");
            c->Execute();
        } catch (const Spinnaker::Exception& e) {
            throw CameraError("Execute " + n + ": " + e.what());
        }
    }

    std::vector<std::string> choices(const std::string& n) override {
        std::vector<std::string> v;
        try {
            CEnumerationPtr e = node(n);
            if (!e) return v;
            NodeList_t entries;
            e->GetEntries(entries);
            for (auto* en : entries) {
                CEnumEntryPtr ee = en;
                if (IsAvailable(ee)) v.push_back(ee->GetSymbolic().c_str());
            }
        } catch (const Spinnaker::Exception&) {
        }
        return v;
    }

    std::vector<Feature> features() override {
        std::vector<Feature> out;
        std::vector<std::string> seen;
        try {
            walk(node("Root"), "Root", out, seen);
        } catch (const Spinnaker::Exception&) {
        }
        return out;
    }

    void walk(CNodePtr catNode, const std::string& catName, std::vector<Feature>& out, std::vector<std::string>& seen) {
        CCategoryPtr cat = catNode;
        if (!cat) return;
        FeatureList_t feats;
        cat->GetFeatures(feats);
        for (auto* iv : feats) {
            CNodePtr p = iv->GetNode();
            std::string fname = p->GetName().c_str();
            if (std::find(seen.begin(), seen.end(), fname) != seen.end()) continue;
            seen.push_back(fname);
            FeatureType k = kindOf(p);
            if (k == FeatureType::Category) {
                walk(p, fname, out, seen);
                continue;
            }
            if (!IsImplemented(p) || !IsAvailable(p)) continue;
            Feature f;
            f.name = fname;
            f.category = catName;
            f.type = k;
            f.readable = IsReadable(p);
            f.writable = IsWritable(p) || k == FeatureType::Command;
            if (f.readable && k != FeatureType::Command) {
                try {
                    f.value = CValuePtr(p)->ToString().c_str();
                } catch (const Spinnaker::Exception& e) {
                    f.value = std::string("<") + e.what() + ">";
                }
            }
            if (k == FeatureType::Enumeration) f.choices = choices(fname);
            if (k == FeatureType::Float) {
                try {
                    f.unit = CFloatPtr(p)->GetUnit().c_str();
                } catch (...) {
                }
            }
            f.description = p->GetDescription().c_str();
            out.push_back(std::move(f));
        }
    }

    void start() override {
        if (acquiring_) return;
        try {
            CEnumerationPtr am = node("AcquisitionMode");
            if (IsAvailable(am) && IsWritable(am)) am->FromString("Continuous");
            cam_->BeginAcquisition();
            acquiring_ = true;
        } catch (const Spinnaker::Exception& e) {
            throw CameraError(std::string("Start acquisition: ") + e.what());
        }
    }

    void stop() override {
        if (!acquiring_) return;
        acquiring_ = false;
        try {
            cam_->EndAcquisition();
        } catch (...) {
        }
    }

    bool grab(RawFrame& out, double timeoutS) override {
        if (!acquiring_) throw CameraError("Not streaming");
        ImagePtr img;
        try {
            img = cam_->GetNextImage(static_cast<uint64_t>(timeoutS * 1000));
        } catch (const Spinnaker::Exception& e) {
            if (e.GetError() == SPINNAKER_ERR_TIMEOUT) return false;
            ++fail_;
            return false;
        }
        bool ok = false;
        try {
            if (!img->IsIncomplete()) {
                int w = int(img->GetWidth()), h = int(img->GetHeight());
                size_t need = size_t(w) * h * 2;
                if (img->GetBufferSize() >= need) {
                    out.width = w;
                    out.height = h;
                    out.data.resize(size_t(w) * h);
                    std::memcpy(out.data.data(), img->GetData(), need);
                    out.hostTimeNs = nowNs();
                    out.cameraTimestamp = img->GetTimeStamp();
                    out.frameId = img->GetFrameID();
                    ok = true;
                }
            }
            img->Release();
        } catch (const Spinnaker::Exception&) {
            ok = false;
        }
        ok ? ++ok_ : ++fail_;
        return ok;
    }

    std::string stats() override { return "ok " + std::to_string(ok_) + ", failed " + std::to_string(fail_); }

private:
    SystemPtr sys_;
    CameraList list_;
    CameraPtr cam_;
    INodeMap* nm_ = nullptr;
    bool acquiring_ = false;
    uint64_t ok_ = 0, fail_ = 0;
};

}  // namespace

std::unique_ptr<Backend> makeSpinnakerBackend() { return std::make_unique<SpinnakerBackend>(); }

}  // namespace a70

#else
namespace a70 {
std::unique_ptr<Backend> makeSpinnakerBackend() { return nullptr; }
}  // namespace a70
#endif
