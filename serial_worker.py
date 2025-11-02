# serial_worker.py
# Background serial reader + command sender for Flask app.

import threading
import time
import queue
import serial
import serial.tools.list_ports

from parser import parse_line

BAUDRATE = 115200
READ_TIMEOUT = 0.2
KEEP_LAST = 500  # keep up to this many parsed samples

def list_serial_ports():
    return [p.device for p in serial.tools.list_ports.comports()]

class SerialWorker:
    def __init__(self, port=None, baud=BAUDRATE):
        self.port = port
        self.baud = baud
        self.ser = None
        self.thread = None
        self._stop = threading.Event()
        self.line_queue = queue.Queue()
        self.parsed_buffer = []  # list of dicts (bounded)
        self.lock = threading.Lock()
        self.status = "Disconnected"
        self._connect_attempt_lock = threading.Lock()

    def connect(self, port):
        with self._connect_attempt_lock:
            self.disconnect()
            try:
                self.ser = serial.Serial(port, self.baud, timeout=READ_TIMEOUT)
                self.port = port
                self.status = f"Connected: {port}"
                self._stop.clear()
                self.thread = threading.Thread(target=self._read_loop, daemon=True)
                self.thread.start()
                return True, ""
            except Exception as e:
                self.ser = None
                self.status = f"Error: {e}"
                return False, str(e)

    def disconnect(self):
        self._stop.set()
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=0.5)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.status = "Disconnected"

    def _read_loop(self):
        while not self._stop.is_set():
            try:
                if not self.ser or not self.ser.is_open:
                    self.status = "Lost connection"
                    break
                raw = self.ser.readline()
                if not raw:
                    continue
                try:
                    line = raw.decode("utf-8", errors="replace").strip()
                except:
                    line = raw.decode("latin1", errors="replace").strip()
                if line:
                    parsed = parse_line(line)
                    timestamp = time.time()
                    entry = {"raw": line, "ts": timestamp, "parsed": parsed}
                    # push to buffer
                    with self.lock:
                        self.parsed_buffer.append(entry)
                        if len(self.parsed_buffer) > KEEP_LAST:
                            self.parsed_buffer = self.parsed_buffer[-KEEP_LAST:]
                    # put raw line for SSE immediate streaming
                    try:
                        self.line_queue.put(entry, block=False)
                    except queue.Full:
                        pass
            except Exception as e:
                self.status = f"Read error: {e}"
                time.sleep(0.5)
        self.status = "Stopped"

    def get_recent(self, n=200):
        with self.lock:
            return list(self.parsed_buffer[-n:])

    def get_latest(self):
        with self.lock:
            if self.parsed_buffer:
                return self.parsed_buffer[-1]
            return None

    def get_next_line(self, timeout=1.0):
        try:
            return self.line_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def send_command(self, cmd: str):
        if not self.ser or not self.ser.is_open:
            return False, "Not connected"
        try:
            out = (cmd.strip() + "\n").encode("utf-8")
            self.ser.write(out)
            return True, ""
        except Exception as e:
            return False, str(e)