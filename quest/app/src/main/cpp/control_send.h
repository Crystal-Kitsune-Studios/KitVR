#pragma once

#include <errno.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>

namespace kitvr {

inline bool sendControlPacket(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            if (n == 0) errno = EPIPE;
            return false;
        }
    }
    return true;
}

}  // namespace kitvr
