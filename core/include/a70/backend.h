#pragma once
// Generic GenICam transport. Knows nothing about FLIR — that lives in camera.h.
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "a70/types.h"

namespace a70 {

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;

    // discovery / lifecycle
    virtual std::vector<DeviceInfo> listDevices() = 0;
    virtual DeviceInfo open(const std::string& id) = 0;  // "" = first camera
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // features (thread-safety: callers serialise through A70Camera's mutex)
    virtual bool has(const std::string& name) = 0;
    virtual FeatureType type(const std::string& name) = 0;
    virtual std::string getString(const std::string& name) = 0;   // any feature as text
    virtual double getFloat(const std::string& name);
    virtual int64_t getInt(const std::string& name);
    virtual void setString(const std::string& name, const std::string& value) = 0;
    virtual void setFloat(const std::string& name, double v);
    virtual void setInt(const std::string& name, int64_t v);
    virtual void execute(const std::string& name) = 0;
    virtual std::vector<std::string> choices(const std::string& name) { return {}; }
    virtual std::vector<Feature> features() { return {}; }

    // streaming (grab() is called from the acquisition thread only)
    virtual void configureStream() {}
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool grab(RawFrame& out, double timeoutS) = 0;  // false on timeout / incomplete
    virtual std::string stats() { return {}; }
};

struct BackendEntry {
    std::string key;     // "aravis", "spinnaker", "sim"
    std::string label;
};

std::vector<BackendEntry> compiledBackends();
std::string defaultBackend();                       // best compiled-in real backend, else "sim"
std::unique_ptr<Backend> makeBackend(const std::string& key);

// factories (defined in each backend's .cpp; return nullptr if not compiled in)
std::unique_ptr<Backend> makeSimBackend();
std::unique_ptr<Backend> makeAravisBackend();
std::unique_ptr<Backend> makeSpinnakerBackend();

}  // namespace a70
