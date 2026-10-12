#pragma once
// systemd service notification (Type=notify + WatchdogSec) without linking libsystemd.
// If the process hangs and stops pinging, systemd kills and restarts it.
// No-op when not started by systemd (or on Windows).
#include <string>

namespace a70 {

class SystemdNotifier {
public:
    SystemdNotifier();
    bool enabled() const { return !socket_.empty(); }
    double watchdogIntervalS() const { return wdS_; }  // 0 = watchdog not configured
    void ready();
    void ping();
    void status(const std::string& s);
    void stopping();

private:
    void send(const std::string& msg);
    std::string socket_;
    double wdS_ = 0;
};

}  // namespace a70
