# parser.py
# tolerant parser for the Arduino serial lines (CSV or JSON-like)

import json
import time

def parse_line(line: str):
    s = line.strip()
    if not s:
        return None

    # JSON-ish
    if s.startswith("{") and s.endswith("}"):
        try:
            obj = json.loads(s)
            parsed = {k.strip().lower(): v for k, v in obj.items()}
            return parsed
        except Exception:
            try:
                inner = s.strip("{} ")
                kvs = inner.split(",")
                parsed = {}
                for kv in kvs:
                    if ":" in kv:
                        k, v = kv.split(":", 1)
                        parsed[k.strip().lower()] = _try_numeric_str(v.strip().strip('"').strip("'"))
                return parsed if parsed else None
            except Exception:
                return None

    # CSV or comma-separated tokens
    parts = [p.strip() for p in s.split(",")]
    if len(parts) >= 4:
        # If 6 or more parts: ts, voltage, current, power, pred_current, pred_power
        if len(parts) >= 6 and _is_number(parts[1]) and _is_number(parts[2]):
            return {
                "ts": float(parts[0]) if _is_number(parts[0]) else time.time(),
                "voltage": float(parts[1]),
                "current": float(parts[2]),
                "power": float(parts[3]) if _is_number(parts[3]) else None,
                "pred_current": float(parts[4]) if _is_number(parts[4]) else None,
                "pred_power": float(parts[5]) if _is_number(parts[5]) else None
            }
        # 4-col fallback: voltage,current,power,pred_current?
        if len(parts) == 4 and all(_is_number(x) for x in parts[:3]):
            parsed = {
                "ts": time.time(),
                "voltage": float(parts[0]),
                "current": float(parts[1]),
                "power": float(parts[2]),
            }
            if _is_number(parts[3]):
                parsed["pred_current"] = float(parts[3])
            return parsed

        # last resort: extract numeric tokens
        nums = [float(p) for p in parts if _is_number(p)]
        if len(nums) >= 3:
            out = {"ts": time.time(), "voltage": nums[0], "current": nums[1], "power": nums[2]}
            if len(nums) >= 5:
                out["pred_current"], out["pred_power"] = nums[3], nums[4]
            return out

    return None

def _is_number(s):
    try:
        float(s)
        return True
    except:
        return False

def _try_numeric_str(s):
    try:
        if "." in s or "e" in s.lower():
            return float(s)
        return int(s)
    except:
        return s