# run_flask.py
# Flask webapp entrypoint

import time
import json
import os
from flask import Flask, render_template, Response, request, jsonify, send_from_directory
from serial_worker import SerialWorker, list_serial_ports
from blynk_client import BlynkBridge

app = Flask(__name__, static_folder="static", template_folder="static")

# single global worker
worker = SerialWorker()
_blynk = None

def _maybe_start_blynk():
    global _blynk
    token = os.getenv("BLYNK_TOKEN", "").strip()
    if not token:
        # try to read from local file 'blynk_token.txt'
        try:
            with open("blynk_token.txt", "r", encoding="utf-8") as f:
                token_file = f.read().strip()
            if token_file:
                os.environ["BLYNK_TOKEN"] = token_file
                token = token_file
        except Exception:
            token = ""
    if token and _blynk is None:
        _blynk = BlynkBridge(worker, token)
        _blynk.start()

_maybe_start_blynk()

@app.route("/")
def index():
    return send_from_directory("static", "index.html")

@app.route("/ports")
def ports():
    return jsonify({"ports": list_serial_ports(), "status": worker.status})

@app.route("/connect", methods=["POST"])
def connect():
    data = request.json or {}
    port = data.get("port")
    if not port:
        return jsonify({"ok": False, "error": "No port specified"}), 400
    ok, err = worker.connect(port)
    if ok:
        return jsonify({"ok": True})
    return jsonify({"ok": False, "error": err}), 500

@app.route("/disconnect", methods=["POST"])
def disconnect():
    worker.disconnect()
    return jsonify({"ok": True})

@app.route("/cmd", methods=["POST"])
def cmd():
    data = request.json or {}
    command = data.get("cmd")
    if not command:
        return jsonify({"ok": False, "error": "No command"}), 400
    ok, err = worker.send_command(command)
    if ok:
        return jsonify({"ok": True})
    return jsonify({"ok": False, "error": err}), 500

# SSE stream for pushing data events
@app.route("/stream")
def stream():
    def event_stream():
        # first send a ping with last N samples
        last = worker.get_recent(200)
        header = {"type": "history", "data": [e for e in last]}
        yield f"data: {json.dumps(header)}\n\n"
        while True:
            entry = worker.get_next_line(timeout=5.0)
            if entry:
                payload = {"type": "sample", "data": entry}
                yield f"data: {json.dumps(payload)}\n\n"
            else:
                # heartbeat to keep connection alive
                yield "data: {}\n\n"
    return Response(event_stream(), mimetype="text/event-stream")

@app.route("/status")
def status():
    return jsonify({"status": worker.status})

if __name__ == "__main__":
    # run local dev server (not for production)
    app.run(host="0.0.0.0", port=5000, threaded=True)