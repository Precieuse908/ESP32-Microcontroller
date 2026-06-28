"""!/usr/bin/env python3"""
"""
esp32cam_receiver.py
====================
UDP receiver for the ESP32-CAM low-latency streaming firmware.

Reassembles fragmented JPEG frames and displays them using OpenCV.
Also opens a WebSocket control channel so you can send commands
to the camera (change quality, resolution, FPS, etc.) from Python.

Requirements:
    pip install opencv-python websockets numpy

Usage:
    python esp32cam_receiver.py \
        --host 0.0.0.0 \
        --port 12345 \
        --ws  ws://192.168.1.xxx:8080

Controls (press in OpenCV window):
    q  → quit
    p  → pause / resume stream
    +  → increase quality (reduce JPEG compression)
    -  → decrease quality (increase JPEG compression)
    f  → toggle flash LED
    s  → print stats to console
"""

import argparse
import asyncio
import collections
import json
import socket
import struct
import threading
import time
from typing import Dict, List, Optional

import cv2
import numpy as np

# ──────────────────────────────────────────────────────────────
#  Frame header layout (must match FrameHeader in .ino)
#  Format string '<IHH' = little-endian: uint32 + uint16 + uint16
# ──────────────────────────────────────────────────────────────
HEADER_FMT    = '<IHH'
HEADER_SIZE   = struct.calcsize(HEADER_FMT)   # 8 bytes

# How many incomplete frames to keep before discarding old ones
# (prevents unbounded memory growth on packet loss)
MAX_BUFFERED_FRAMES = 10


class FrameBuffer:
    """
    Accumulates UDP chunks for a single frame identified by frame_id.
    Once all expected chunks arrive, reassemble() returns the JPEG bytes.
    """
    def __init__(self, frame_id: int, total_chunks: int):
        self.frame_id     = frame_id
        self.total_chunks = total_chunks
        self.chunks: Dict[int, bytes] = {}   # chunk_idx → payload bytes
        self.received_at  = time.monotonic()

    def add_chunk(self, chunk_idx: int, payload: bytes) -> None:
        self.chunks[chunk_idx] = payload

    def is_complete(self) -> bool:
        return len(self.chunks) == self.total_chunks

    def reassemble(self) -> bytes:
        """Concatenate chunks in order to rebuild the full JPEG."""
        return b''.join(self.chunks[i] for i in range(self.total_chunks))

    def age(self) -> float:
        """Seconds since first chunk arrived (used for timeout eviction)."""
        return time.monotonic() - self.received_at


class UDPReceiver:
    """
    Listens on a UDP socket, reassembles JPEG frames,
    and exposes the latest frame via get_latest_frame().
    Runs on a dedicated background thread.
    """

    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port

        # Ordered dict of active FrameBuffer objects keyed by frame_id
        self._buffers: Dict[int, FrameBuffer] = {}

        # Latest fully-assembled JPEG bytes (thread-safe via lock)
        self._latest_jpeg: Optional[bytes] = None
        self._lock = threading.Lock()

        # Stats
        self.frames_received  = 0
        self.frames_dropped   = 0
        self.bytes_received   = 0
        self._fps_timestamps: collections.deque = collections.deque(maxlen=30)

        self._running = False

    def start(self) -> None:
        """Start the UDP listener thread."""
        self._running = True
        t = threading.Thread(target=self._listen_loop, daemon=True)
        t.start()
        print(f"[UDP] Listening on {self.host}:{self.port}")

    def stop(self) -> None:
        self._running = False

    def get_latest_frame(self) -> Optional[np.ndarray]:
        """
        Return the latest decoded BGR frame as a NumPy array,
        or None if no frame has arrived yet.
        Thread-safe — safe to call from the main/display thread.
        """
        with self._lock:
            jpeg = self._latest_jpeg
        if jpeg is None:
            return None
        # Decode JPEG → BGR numpy array
        arr = np.frombuffer(jpeg, dtype=np.uint8)
        return cv2.imdecode(arr, cv2.IMREAD_COLOR)

    def fps(self) -> float:
        """Rolling FPS over the last 30 decoded frames."""
        ts = list(self._fps_timestamps)
        if len(ts) < 2:
            return 0.0
        return (len(ts) - 1) / (ts[-1] - ts[0])

    # ── Internal ────────────────────────────────────────────

    def _listen_loop(self) -> None:
        """UDP receive loop — runs on background thread."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)  # 2 MB rx buf
        sock.bind((self.host, self.port))
        sock.settimeout(0.5)  # non-blocking poll, allows clean shutdown

        while self._running:
            try:
                data, addr = sock.recvfrom(65535)
            except socket.timeout:
                # Evict stale frame buffers (older than 1 second)
                self._evict_old_buffers(max_age=1.0)
                continue

            self.bytes_received += len(data)

            if len(data) < HEADER_SIZE:
                continue  # malformed packet

            # Unpack header
            frame_id, chunk_idx, total_chunks = struct.unpack_from(
                HEADER_FMT, data, 0
            )
            payload = data[HEADER_SIZE:]

            # Get or create FrameBuffer for this frame_id
            if frame_id not in self._buffers:
                # Evict oldest buffer if we're at capacity
                if len(self._buffers) >= MAX_BUFFERED_FRAMES:
                    oldest_id = next(iter(self._buffers))
                    del self._buffers[oldest_id]
                    self.frames_dropped += 1
                self._buffers[frame_id] = FrameBuffer(frame_id, total_chunks)

            buf = self._buffers[frame_id]
            buf.add_chunk(chunk_idx, payload)

            if buf.is_complete():
                jpeg = buf.reassemble()
                del self._buffers[frame_id]

                # Update shared latest frame
                with self._lock:
                    self._latest_jpeg = jpeg

                self.frames_received += 1
                self._fps_timestamps.append(time.monotonic())

        sock.close()

    def _evict_old_buffers(self, max_age: float) -> None:
        """Remove incomplete frame buffers older than max_age seconds."""
        stale = [fid for fid, buf in self._buffers.items()
                 if buf.age() > max_age]
        for fid in stale:
            del self._buffers[fid]
            self.frames_dropped += 1


class WSController:
    """
    WebSocket client that sends control commands to the ESP32-CAM.
    All methods are fire-and-forget (non-blocking from caller's view).
    Runs asyncio on a background thread.
    """

    def __init__(self, ws_url: str):
        self.ws_url   = ws_url
        self._ws      = None
        self._loop    = asyncio.new_event_loop()
        self._connected = False

    def start(self) -> None:
        t = threading.Thread(target=self._run_loop, daemon=True)
        t.start()

    def send(self, msg: dict) -> None:
        if self._connected and self._ws:
            asyncio.run_coroutine_threadsafe(
                self._ws.send(json.dumps(msg)), self._loop
            )

    def pause(self):   self.send({"cmd": "pause"})
    def resume(self):  self.send({"cmd": "resume"})
    def flash_on(self):  self.send({"cmd": "flash", "val": 1})
    def flash_off(self): self.send({"cmd": "flash", "val": 0})

    def set_quality(self, q: int):
        self.send({"cmd": "quality", "val": max(10, min(63, q))})

    def set_fps(self, fps: int):
        self.send({"cmd": "fps", "val": max(1, min(60, fps))})

    def _run_loop(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect())

    async def _connect(self) -> None:
        import websockets
        print(f"[WS] Connecting to {self.ws_url}")
        try:
            # BEFORE
            # async with websockets.connect(self.ws_url) as ws:
            ### The ESP32 main loop is too busy sending UDP frames to respond to WebSocket ping 
            ### frames in time. Fix this in esp32cam_receiver.py — find the _connect method in 
            ### WSController and add ping_interval=None:
            # AFTER
            async with websockets.connect(self.ws_url, ping_interval=None) as ws:
                self._ws = ws
                self._connected = True
                print("[WS] Connected to ESP32-CAM control channel.")
                async for message in ws:
                    data = json.loads(message)
                    print(f"[WS] ← {data}")
        except Exception as e:
            print(f"[WS] Connection error: {e}")
        finally:
            self._connected = False


def draw_overlay(frame: np.ndarray, fps: float, stats: dict) -> np.ndarray:
    """Draw a minimal HUD (FPS, RSSI, quality) on the frame."""
    h, w = frame.shape[:2]
    overlay = frame.copy()

    # Semi-transparent black bar at top
    cv2.rectangle(overlay, (0, 0), (w, 32), (0, 0, 0), -1)
    cv2.addWeighted(overlay, 0.6, frame, 0.4, 0, frame)

    text = (f"FPS: {fps:.1f}  |  "
            f"RSSI: {stats.get('rssi', '?')} dBm  |  "
            f"Q: {stats.get('quality', '?')}  |  "
            f"Heap: {stats.get('heap', 0)//1024} KB")
    cv2.putText(frame, text, (8, 20),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 180), 1, cv2.LINE_AA)
    return frame


def main():
    parser = argparse.ArgumentParser(description="ESP32-CAM UDP receiver")
    parser.add_argument("--host", default="0.0.0.0",    help="Local bind address")
    parser.add_argument("--port", type=int, default=12345, help="UDP port")
    parser.add_argument("--ws",   default=None,         help="WS control URL, e.g. ws://192.168.1.x:8080")
    args = parser.parse_args()

    # ── Start UDP receiver ─────────────────────────────────
    receiver = UDPReceiver(args.host, args.port)
    receiver.start()

    # ── Start WS controller (optional) ────────────────────
    ctrl = None
    if args.ws:
        ctrl = WSController(args.ws)
        ctrl.start()

    # ── Display loop ───────────────────────────────────────
    print("[Display] Press 'q' to quit. Controls: +/- quality, p pause, f flash, s stats")

    quality    = 20
    flash_on   = False
    stats_data = {}
    paused     = False

    cv2.namedWindow("ESP32-CAM Stream", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("ESP32-CAM Stream", 640, 480)

    # Latency measurement
    last_frame_time = time.monotonic()

    while True:
        frame = receiver.get_latest_frame()

        if frame is not None:
            now = time.monotonic()
            latency_ms = (now - last_frame_time) * 1000
            last_frame_time = now

            # Build stats dict for overlay
            stats_data.update({
                "latency_ms": f"{latency_ms:.1f}",
                "quality": quality,
            })

            frame = draw_overlay(frame, receiver.fps(), stats_data)
            cv2.imshow("ESP32-CAM Stream", frame)
        else:
            # Show waiting screen
            blank = np.zeros((240, 320, 3), dtype=np.uint8)
            cv2.putText(blank, "Waiting for stream...", (20, 120),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (100, 200, 100), 1)
            cv2.imshow("ESP32-CAM Stream", blank)

        # ── Key handling ───────────────────────────────────
        key = cv2.waitKey(1) & 0xFF

        if key == ord('q'):
            break

        elif key == ord('p') and ctrl:
            if paused:
                ctrl.resume(); paused = False
            else:
                ctrl.pause();  paused = True

        elif key == ord('+') and ctrl:
            quality = max(10, quality - 3)  # lower number = better quality
            ctrl.set_quality(quality)
            print(f"[Ctrl] Quality → {quality}")

        elif key == ord('-') and ctrl:
            quality = min(63, quality + 3)  # higher number = faster
            ctrl.set_quality(quality)
            print(f"[Ctrl] Quality → {quality}")

        elif key == ord('f') and ctrl:
            flash_on = not flash_on
            if flash_on: ctrl.flash_on()
            else:        ctrl.flash_off()

        elif key == ord('s'):
            print(f"\n[Stats] Frames received : {receiver.frames_received}")
            print(f"[Stats] Frames dropped  : {receiver.frames_dropped}")
            print(f"[Stats] Bytes received  : {receiver.bytes_received/1024:.1f} KB")
            print(f"[Stats] FPS             : {receiver.fps():.2f}")
            if ctrl: ctrl.send({"cmd": "stats"})

    # ── Cleanup ────────────────────────────────────────────
    receiver.stop()
    cv2.destroyAllWindows()
    print("[Receiver] Shutdown complete.")


if __name__ == "__main__":
    main()
