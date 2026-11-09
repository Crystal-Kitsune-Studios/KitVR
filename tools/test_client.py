#!/usr/bin/env python3
"""KitVR test client. Stands in for the Quest so you can check the server without a headset.

It connects to the server, sends tracking packets (which tells the server to start streaming),
reassembles the video, counts audio packets, and prints stats every second. With --out it saves
the video so you can watch it:  ffplay -f hevc test.h265

Usage:  python3 test_client.py [--host 127.0.0.1] [--seconds 10] [--out test.h265]
Needs only the Python standard library. Matches docs/PROTOCOL.md (protocol v1).
"""
import argparse
import socket
import struct
import sys
import time

CONTROL_PORT = 9943
MEDIA_PORT = 9944

VIDEO_HDR = struct.Struct("<BBBBIQHHHH")  # 24 bytes
AUDIO_HDR = struct.Struct("<BBHQHH")      # 16 bytes
assert VIDEO_HDR.size == 24 and AUDIO_HDR.size == 16


def frame_msg(tag, body=b""):
    return struct.pack("<IB", len(body) + 1, tag) + body


def hello():
    name = b"test-client"
    return frame_msg(1, struct.pack("<HB", 1, len(name)) + name)


def ping(ts_us):
    return frame_msg(5, struct.pack("<Q", ts_us))


def tracking(seq, ts_us):
    head = struct.pack("<7f", 0, 0, 0, 0, 0, 0, 1)
    pkt = struct.pack("<BBHQ", 2, 0, seq, ts_us) + head
    return pkt + bytes(136 - len(pkt))


def now_us():
    return int(time.monotonic() * 1_000_000)


class Control:
    def __init__(self, host):
        self.sock = socket.create_connection((host, CONTROL_PORT), timeout=5)
        self.sock.setblocking(False)
        self.buf = b""
        self.stream_cfg = None
        self.audio_cfg = None
        self.server_name = None
        self.rtts = []

    def send(self, data):
        self.sock.sendall(data)

    def poll(self):
        try:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("server closed the control connection")
            self.buf += data
        except BlockingIOError:
            pass
        while len(self.buf) >= 5:
            (length,) = struct.unpack_from("<I", self.buf)
            if len(self.buf) < 4 + length:
                break
            tag = self.buf[4]
            body = self.buf[5:4 + length]
            self.buf = self.buf[4 + length:]
            if tag == 1:
                version, n = struct.unpack_from("<HB", body)
                self.server_name = body[3:3 + n].decode(errors="replace")
                print(f"server hello: {self.server_name} (protocol {version})")
            elif tag == 2:
                self.stream_cfg = struct.unpack_from("<BHHHI", body)
                codec, w, h, fps, kbps = self.stream_cfg
                print(f"stream config: codec {codec} {w}x{h} @{fps}, {kbps} kbps")
            elif tag == 8:
                self.audio_cfg = struct.unpack_from("<IBHB", body)
                rate, ch, frame, mic = self.audio_cfg
                print(f"audio config: {rate} Hz, {ch} ch to headset, {frame} samples/packet, {mic} mic ch")
            elif tag == 6:
                (ts,) = struct.unpack_from("<Q", body)
                self.rtts.append((now_us() - ts) / 1000.0)
            elif tag == 7:
                raise ConnectionError("server sent Bye")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--out", help="write the received HEVC stream to this file")
    args = ap.parse_args()

    try:
        ctl = Control(args.host)
    except OSError as e:
        print(f"cannot connect to {args.host}:{CONTROL_PORT}: {e}")
        print("is kitvr-server running?")
        return 1
    ctl.send(hello())

    deadline = time.time() + 5
    while ctl.stream_cfg is None and time.time() < deadline:
        ctl.poll()
        time.sleep(0.01)
    if ctl.stream_cfg is None:
        print("FAIL: no StreamConfig received")
        return 1

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    udp.settimeout(0.005)
    server = (args.host, MEDIA_PORT)

    out = open(args.out, "wb") if args.out else None
    got_key = False
    asm = {}  # frame_id -> [count, {idx: bytes}, keyframe]
    last_done = None

    frames = keyframes = lost = audio_pkts = video_bytes = 0
    first_video = None
    seq = 0
    t0 = time.time()
    next_stat = t0 + 1
    next_ping = t0
    next_track = t0
    w_frames = w_bytes = w_audio = 0

    while time.time() - t0 < args.seconds:
        now = time.time()
        if now >= next_track:  # about 90 Hz
            udp.sendto(tracking(seq, now_us()), server)
            seq = (seq + 1) & 0xFFFF
            next_track = now + 1 / 90
        if now >= next_ping:
            ctl.send(ping(now_us()))
            next_ping = now + 1
        try:
            ctl.poll()
            data, _ = udp.recvfrom(2048)
        except socket.timeout:
            data = None
        except ConnectionError as e:
            print(e)
            break

        if data:
            kind = data[0]
            if kind == 3 and len(data) >= 16:
                audio_pkts += 1
                w_audio += 1
            elif kind == 1 and len(data) >= 24:
                (_, flags, sidx, scount, fid, _pose, pidx, pcount, plen, _r) = VIDEO_HDR.unpack_from(data)
                payload = data[24:24 + plen]
                if first_video is None:
                    first_video = time.time() - t0
                entry = asm.setdefault(fid, [pcount, {}, bool(flags & 1)])
                entry[1][pidx] = payload
                if len(entry[1]) == entry[0]:
                    au = b"".join(entry[1][i] for i in range(entry[0]))
                    frames += 1
                    w_frames += 1
                    video_bytes += len(au)
                    w_bytes += len(au)
                    if entry[2]:
                        keyframes += 1
                        got_key = True
                    if out and got_key:
                        out.write(au)
                    # anything older than this frame that never finished counts as lost
                    for old in [k for k in asm if k < fid]:
                        if len(asm[old][1]) < asm[old][0]:
                            lost += 1
                        del asm[old]
                    del asm[fid]
                    last_done = fid

        if time.time() >= next_stat:
            print(f"  video {w_frames:3d} fps, {w_bytes * 8 / 1e6:5.1f} Mbps | audio {w_audio:4d} pkt/s | lost frames {lost}")
            w_frames = w_bytes = w_audio = 0
            next_stat += 1

    if out:
        out.close()
    try:
        ctl.send(frame_msg(7))
    except OSError:
        pass

    print("\nsummary")
    print(f"  frames received: {frames} ({keyframes} keyframes), lost: {lost}")
    print(f"  first video after: {first_video:.2f}s" if first_video is not None else "  first video: never")
    print(f"  audio packets: {audio_pkts}")
    if ctl.rtts:
        print(f"  control rtt: {sum(ctl.rtts) / len(ctl.rtts):.2f} ms avg over {len(ctl.rtts)} pings")
    if args.out:
        print(f"  saved video to {args.out}   (watch with: ffplay -f hevc {args.out})")

    ok = frames > 0
    print("PASS" if ok else "FAIL: no video frames were completed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
