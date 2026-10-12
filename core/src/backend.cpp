#include "a70/backend.h"

#include <cmath>

namespace a70 {

double Backend::getFloat(const std::string& n) {
    try {
        return std::stod(getString(n));
    } catch (const std::invalid_argument&) {
        throw CameraError("Feature " + n + " is not numeric");
    }
}

int64_t Backend::getInt(const std::string& n) {
    try {
        return std::stoll(getString(n));
    } catch (const std::invalid_argument&) {
        throw CameraError("Feature " + n + " is not an integer");
    }
}

void Backend::setFloat(const std::string& n, double v) { setString(n, fmtDouble(v, 12)); }
void Backend::setInt(const std::string& n, int64_t v) { setString(n, std::to_string(v)); }

std::vector<BackendEntry> compiledBackends() {
    std::vector<BackendEntry> v;
#ifdef A70_HAVE_ARAVIS
    v.push_back({"aravis", "Aravis (GigE Vision, Linux)"});
#endif
#ifdef A70_HAVE_SPINNAKER
    v.push_back({"spinnaker", "Spinnaker SDK (GigE Vision)"});
#endif
    v.push_back({"sim", "Simulator (no camera)"});
    return v;
}

std::string defaultBackend() {
#if defined(A70_HAVE_SPINNAKER) && defined(_WIN32)
    return "spinnaker";
#elif defined(A70_HAVE_ARAVIS)
    return "aravis";
#elif defined(A70_HAVE_SPINNAKER)
    return "spinnaker";
#else
    return "sim";
#endif
}

std::unique_ptr<Backend> makeBackend(const std::string& key) {
    std::unique_ptr<Backend> b;
    if (key == "sim") b = makeSimBackend();
    else if (key == "aravis") b = makeAravisBackend();
    else if (key == "spinnaker") b = makeSpinnakerBackend();
    if (!b) throw CameraError("Backend '" + key + "' is not compiled into this build");
    return b;
}

}  // namespace a70
