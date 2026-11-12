#include "control_send.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

int main() {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 1;
    const uint8_t message[] = {1, 2, 3, 4};
    if (!kitvr::sendControlPacket(sockets[0], message, sizeof(message))) return 2;
    uint8_t received[sizeof(message)]{};
    if (recv(sockets[1], received, sizeof(received), MSG_WAITALL) != sizeof(received) ||
        !std::equal(std::begin(message), std::end(message), std::begin(received))) return 3;

    int bufferSize = 4096;
    if (setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) != 0) return 4;
    std::vector<uint8_t> large(1024 * 1024, 42);
    if (kitvr::sendControlPacket(sockets[0], large.data(), large.size()) ||
        (errno != EAGAIN && errno != EWOULDBLOCK)) return 5;
    std::vector<uint8_t> prefix(large.size());
    const ssize_t count = recv(sockets[1], prefix.data(), prefix.size(), MSG_DONTWAIT);
    if (count <= 0 || static_cast<size_t>(count) >= large.size() ||
        !std::equal(prefix.begin(), prefix.begin() + count, large.begin())) return 6;

    close(sockets[1]);
    if (kitvr::sendControlPacket(sockets[0], message, sizeof(message))) return 7;
    close(sockets[0]);
    return 0;
}
