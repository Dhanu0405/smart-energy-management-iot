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
        self._consecutive_errors = 0  # Track consecutive "offline" errors
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
                # sleep to avoid busy loop
                self._stop.wait(0.2)
                continue
            now = time.time()
            if now - self._last_push_ts < min_interval:
                # brief sleep to avoid busy spinning
                self._stop.wait(min_interval - (now - self._last_push_ts))
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
            relay_status = 1 if relay_str == "ON" or relay_str == "1" or relay_str == "TRUE" else 0

            self._push_values(voltage, current, power, relay_status)

    def _push_values(self, voltage, current, power, relay):
        # Send individual updates. Always push relay state to keep widget online.
        try:
            if voltage is not None:
                self._session.get(self._base_update, params={"token": self.token, "V1": voltage}, timeout=3)
            if current is not None:
                self._session.get(self._base_update, params={"token": self.token, "V2": current}, timeout=3)
            if power is not None:
                self._session.get(self._base_update, params={"token": self.token, "V3": power}, timeout=3)
            # Always push relay state (even if unchanged) to keep widget showing online
            self._session.get(self._base_update, params={"token": self.token, "V4": relay}, timeout=3)
        except Exception:
            # best-effort; avoid crashing the loop
            pass

    def _poll_loop(self):
        # Adaptive polling: faster when getting errors, slower when working
        base_interval = 3.0
        error_interval = 1.0  # Poll every 1s when we're getting "offline" errors
        while not self._stop.is_set():
            start = time.time()
            desired = self._get_v4()
            if desired is not None:
                # Success - reset error counter and use normal interval
                self._consecutive_errors = 0
                if self._last_seen_rel is None or desired != self._last_seen_rel:
                    self._last_seen_rel = desired
                    self._apply_relay(desired)
                    print(f"[Blynk] Relay state changed to: {desired} (ON)" if desired == 1 else f"[Blynk] Relay state changed to: {desired} (OFF)")
                poll_interval = base_interval
            else:
                # Error - increase error counter and use faster polling
                self._consecutive_errors += 1
                poll_interval = error_interval
                # If we've had many errors, try retrying immediately a few times
                if self._consecutive_errors % 3 == 0:
                    # Every 3rd error, try a few immediate retries
                    for _ in range(3):
                        desired = self._get_v4()
                        if desired is not None:
                            self._consecutive_errors = 0
                            if self._last_seen_rel is None or desired != self._last_seen_rel:
                                self._last_seen_rel = desired
                                self._apply_relay(desired)
                                print(f"[Blynk] Relay state changed to: {desired} (ON)" if desired == 1 else f"[Blynk] Relay state changed to: {desired} (OFF)")
                            poll_interval = base_interval
                            break
                        time.sleep(0.3)  # Brief delay between retries
            
            # sleep remaining
            elapsed = time.time() - start
            to_sleep = max(0.1, poll_interval - elapsed)
            self._stop.wait(to_sleep)

    def _get_v4(self) -> Optional[int]:
        # Use params rather than manual URL composition. Accept a variety of responses.
        params = {"token": self.token, "V4": ""}
        # single attempt logic but handle different response formats robustly
        try:
            r = self._session.get(self._base_get, params=params, timeout=5)
        except Exception:
            return None

        if r.status_code != 200:
            return None

        text = (r.text or "").strip().lower()
        if not text or text == "null":
            return None

        # If the cloud returns an "offline" indicator — treat as not available
        if "offline" in text or "error" in text:
            return None

        # Accept numeric, boolean, and common strings
        # try numeric first
        try:
            val = int(float(text))
            return 1 if val >= 1 else 0
        except Exception:
            pass

        # textual mappings
        if text in ("1", "true", "on", "high"):
            return 1
        if text in ("0", "false", "off", "low"):
            return 0

        # fallback: unknown format
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


