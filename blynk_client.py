import os
import threading
import time
from typing import Optional

import requests


class BlynkBridge:
    """
    Bridges SerialWorker samples to Blynk Cloud (V1=Voltage, V2=Current, V3=Power, V4=Relay status).
    Also polls V4 from Blynk and forwards ON/OFF to SerialWorker via serial commands.
    """

    def __init__(self, worker, token: Optional[str] = None):
        self.worker = worker
        self.token = token or os.getenv("BLYNK_TOKEN", "").strip()
        self._stop = threading.Event()
        self.t_push = None
        self.t_poll = None
        self._last_push_ts = 0.0
        self._last_seen_rel = None  # type: Optional[int]
        self._session = requests.Session()

        self._base_update = "https://blynk.cloud/external/api/update"
        self._base_get = "https://blynk.cloud/external/api/get"

    def start(self):
        if not self.token:
            return False, "Blynk token missing. Set env BLYNK_TOKEN or pass token."
        self._stop.clear()
        self.t_push = threading.Thread(target=self._push_loop, daemon=True)
        self.t_poll = threading.Thread(target=self._poll_loop, daemon=True)
        self.t_push.start()
        self.t_poll.start()
        return True, ""

    def stop(self):
        self._stop.set()
        if self.t_push and self.t_push.is_alive():
            self.t_push.join(timeout=0.5)
        if self.t_poll and self.t_poll.is_alive():
            self.t_poll.join(timeout=0.5)

    def _push_loop(self):
        # Poll latest parsed entry and push to Blynk (rate-limited, no queue contention).
        min_interval = 0.2
        while not self._stop.is_set():
            entry = self.worker.get_latest()
            if not entry:
                time.sleep(0.2)
                continue
            now = time.time()
            if now - self._last_push_ts < min_interval:
                continue
            self._last_push_ts = now
            parsed = entry.get("parsed", {}) or {}

            # Parse values
            try:
                voltage = float(parsed.get("voltage")) if parsed.get("voltage") is not None else None
            except Exception:
                voltage = None
            try:
                current = float(parsed.get("current")) if parsed.get("current") is not None else None
            except Exception:
                current = None
            try:
                power = float(parsed.get("power")) if parsed.get("power") is not None else None
            except Exception:
                power = None

            relay_str = str(parsed.get("relay", "")).upper()
            relay_status = 1 if relay_str == "ON" else 0

            self._push_values(voltage, current, power, relay_status)

    def _push_values(self, voltage, current, power, relay):
        # Send individual updates. Ignore None values.
        try:
            if voltage is not None:
                self._session.get(self._base_update, params={"token": self.token, "V1": voltage}, timeout=3)
            if current is not None:
                self._session.get(self._base_update, params={"token": self.token, "V2": current}, timeout=3)
            if power is not None:
                self._session.get(self._base_update, params={"token": self.token, "V3": power}, timeout=3)
            self._session.get(self._base_update, params={"token": self.token, "V4": relay}, timeout=3)
        except Exception:
            # best-effort; avoid crashing the loop
            pass

    def _poll_loop(self):
        # Poll Blynk V4 every 5 seconds for relay control and forward to device.
        # Only send command when state changes.
        poll_interval = 5.0
        while not self._stop.is_set():
            start = time.time()
            desired = self._get_v4()
            if desired is not None:
                if self._last_seen_rel is None or desired != self._last_seen_rel:
                    self._last_seen_rel = desired
                    self._apply_relay(desired)
            # sleep remaining
            elapsed = time.time() - start
            to_sleep = max(0.2, poll_interval - elapsed)
            self._stop.wait(to_sleep)

    def _get_v4(self) -> Optional[int]:
        # Try several API variants to avoid "Device is offline" due to format mismatch
        variants = [
            f"{self._base_get}?token={self.token}&V4",
            f"{self._base_get}?token={self.token}&pin=V4",
            f"{self._base_get}?token={self.token}&v4",
        ]
        for url in variants:
            try:
                r = self._session.get(url, timeout=4)
                text = (r.text or "").strip()
                if r.ok and text and "offline" not in text.lower():
                    val = int(float(text))
                    return 1 if val >= 1 else 0
            except Exception:
                continue
        return None

    def _apply_relay(self, desired: int):
        # Forward command to SerialWorker
        try:
            if desired == 1:
                self.worker.send_command("RELAY ON")
            else:
                self.worker.send_command("RELAY OFF")
        except Exception:
            pass


