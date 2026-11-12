#include "tracking_sequence.h"

int main() {
    kitvr::TrackingSequence head;
    kitvr::TrackingSequence body;
    if (!head.accept(65534) || !body.accept(10)) return 1;
    if (!head.accept(65535) || !head.accept(0) || !head.accept(2)) return 2;
    if (head.accept(2) || head.accept(1) || head.accept(65535) || head.accept(0x8002)) return 3;
    if (!body.accept(11) || body.accept(10)) return 4;
    head.reset();
    if (!head.accept(1) || !body.accept(12)) return 5;
    return 0;
}
