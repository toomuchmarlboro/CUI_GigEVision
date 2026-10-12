#include "a70/watchdog.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace a70 {

SystemdNotifier::SystemdNotifier() {
#ifndef _WIN32
    if (const char* s = std::getenv("NOTIFY_SOCKET")) socket_ = s;
    if (const char* w = std::getenv("WATCHDOG_USEC")) wdS_ = std::atof(w) / 1e6;
#endif
}

void SystemdNotifier::send(const std::string& msg) {
#ifndef _WIN32
    if (socket_.empty()) return;
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t n = std::min(socket_.size(), sizeof(addr.sun_path) - 1);
    std::memcpy(addr.sun_path, socket_.data(), n);
    if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace
    socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + n);
    ::sendto(fd, msg.data(), msg.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&addr), len);
    ::close(fd);
#else
    (void)msg;
#endif
}

void SystemdNotifier::ready() { send("READY=1"); }
void SystemdNotifier::ping() { send("WATCHDOG=1"); }
void SystemdNotifier::status(const std::string& s) { send("STATUS=" + s); }
void SystemdNotifier::stopping() { send("STOPPING=1"); }

}  // namespace a70
