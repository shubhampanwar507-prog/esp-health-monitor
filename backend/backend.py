import os
import csv
import joblib
import pandas as pd
from flask import Flask, request, jsonify
from datetime import datetime

app = Flask(__name__)

# ------------ Load ML Model ------------
MODEL_PATH = "C:\\Users\\cse\\OneDrive\\Documents\\Arduino\\Esp_Flask_Server\\glucose_range_model.pkl"

if not os.path.isfile(MODEL_PATH):
    raise FileNotFoundError(f"Model file not found: {MODEL_PATH}")

try:
    glucose_model = joblib.load(MODEL_PATH)
    print("Model loaded successfully!")
except Exception as e:
    print("❌ ERROR loading model:", e)
    raise e

latest_glucose = "UNKNOWN"

# ------------ Logging Directory ------------
LOG_DIR = "logs"
os.makedirs(LOG_DIR, exist_ok=True)


# ------------ Safe BP Extraction ------------
def extract_bp(data):
    """
    Selects either Cuff BP if available OR PPG waveform BP as fallback.
    Also returns source for logging.
    """
    sys_peak_ppg = data.get("sys_peak", 0)
    dia_peak_ppg = data.get("dia_peak", 0)

    bpstr = data.get("bp", "")
    cuff_sys = None
    cuff_dia = None

    # Try to parse cuff BP "120/80"
    if isinstance(bpstr, str) and "/" in bpstr:
        parts = bpstr.split("/")
        if len(parts) == 2:
            try:
                cuff_sys = int(parts[0])
                cuff_dia = int(parts[1])
            except:
                pass

    # Choose best available
    if cuff_sys is not None and cuff_dia is not None:
        return cuff_sys, cuff_dia, "CUFF"
    else:
        return sys_peak_ppg, dia_peak_ppg, "PPG"


# ------------ Glucose Prediction ------------
def predict_glucose_range(data):
    sys_bp, dia_bp, src = extract_bp(data)

    df = pd.DataFrame([{
        "PPG_Signal": data.get("ppg_signal", 0),
        "Heart_Rate": data.get("bpm", 0),
        "Systolic_Peak": sys_bp,
        "Diastolic_Peak": dia_bp,
        "Pulse_Area": data.get("pulse_area", 0),

        # User constants for now
        "Age": 22,
        "Gender": 1,
        "Height": 170,
        "Weight": 65,
        "pl": 1
    }])

    pred = glucose_model.predict(df)[0]
    return pred, src


# ------------ Logging ------------
def write_log(data, bp_src):
    today = datetime.now().strftime("%Y-%m-%d")
    filename = os.path.join(LOG_DIR, f"vitals_{today}.csv")

    file_exists = os.path.isfile(filename)

    with open(filename, "a", newline="") as f:
        writer = csv.writer(f)

        if not file_exists:
            writer.writerow([
                "timestamp","user_id",
                "ir","red",
                "spo2_raw","spo2_filtered",
                "bpm","beatAvg",
                "bp",
                "sys_peak","dia_peak","pulse_area",
                "glucose_range",
                "bp_source"
            ])

        writer.writerow([
            datetime.now().isoformat(),
            data.get("user_id", "UNKNOWN"),

            data.get("ir"), data.get("red"),
            data.get("spo2_raw"), data.get("spo2_filtered"),
            data.get("bpm"), data.get("beatAvg"),
            data.get("bp"),

            data.get("sys_peak"), data.get("dia_peak"),
            data.get("pulse_area"),

            data.get("glucose_range"),
            bp_src
        ])


# ------------ Main Upload Endpoint ------------
@app.route("/upload", methods=["POST"])
def upload():
    global latest_glucose

    raw = request.data
    print("\nIncoming RAW:", raw)

    data = request.get_json(silent=True)
    if not data:
        print("❌ JSON parse failed")
        return jsonify({"status": "error", "msg": "Invalid JSON"}), 400

    try:
        glucose_range, bp_src = predict_glucose_range(data)
    except Exception as e:
        print("❌ Prediction failed:", e)
        return jsonify({"status":"error","msg":str(e)}), 500

    data["glucose_range"] = glucose_range
    latest_glucose = glucose_range

    try:
        write_log(data, bp_src)
    except Exception as e:
        print("❌ Log write error:", e)

    print(f"✔ Uploaded OK | User:{data.get('user_id')} | Glucose:{glucose_range} | BP Source:{bp_src}")

    return jsonify({
        "status": "ok",
        "glucose_range": glucose_range,
        "bp_source": bp_src
    })


# ------------ Endpoint for Dashboard to get latest glucose ------------
@app.route("/latest_glucose")
def get_glucose():
    return jsonify(latest_glucose)


# ------------ Run Server ------------
if __name__ == "__main__":
    print("Backend started at http://0.0.0.0:5000")
    app.run(host="0.0.0.0", port=5000, debug=True)
