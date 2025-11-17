📘 ESP-Health-Monitor

An IoT-based real-time health monitoring system using:

ESP32 + MAX30102 (PPG sensor)

Blood pressure input (Cuff or PPG extracted peaks)

Flask backend server

Machine Learning glucose prediction model

Local dashboard support (future integration)

This system captures vitals from ESP32, sends them to a Python backend, predicts glucose ranges using an ML model, and logs the data for dashboards & analysis.

🚀 Features
1. ESP32 (IoT Device)

Reads IR, RED, PPG waveform

Calculates:

Heart rate (BPM)

SpO2

PPG Peaks (Systolic, Diastolic)

Pulse area

Sends JSON packets to backend over Wi-Fi

Supports both cuff BP & PPG-derived BP

2. Flask Backend

Receives vitals through /upload API

Extracts proper BP source (Cuff first → else PPG)

Predicts glucose range using
glucose_range_model.pkl

Logs data automatically into daily CSV logs

Exposes /latest_glucose for dashboards

3. Machine Learning Model

Trained to classify glucose range using:

PPG parameters

Heart rate

Peaks

Pulse characteristics

User constants (age, height, weight)

4. Logging

Each upload is stored in:

backend/logs/vitals_YYYY-MM-DD.csv


With fields:

timestamp

user ID

raw PPG

SpO2 raw / filtered

BPM

BP values

glucose range

BP data source (CUFF / PPG)

📂 Project Structure
esp-health-monitor/
│
├── esp32/
│   └── sketch_nov13a.ino
│
├── backend/
│   ├── backend.py
│   ├── glucose_range_model.pkl
│   ├── requirements.txt
│   └── logs/
│
├── dashboard/         # (future feature)
│
└── README.md

🔌 How It Works
ESP32 → Backend → ML → Dashboard

ESP32 collects vitals & PPG waveform

Sends JSON to Flask server

Backend preprocesses & selects BP

ML predicts glucose range

Server logs data

Dashboard (future) displays vitals in charts

🛠️ Backend Installation
1. Create virtual environment
python -m venv venv
venv\Scripts\activate

2. Install dependencies
pip install -r requirements.txt

3. Start the server
python backend.py


Server runs on:

http://0.0.0.0:5000

📡 API Endpoints
POST /upload

Send JSON from ESP32:

{
  "user_id": "u001",
  "ir": 123456,
  "red": 112233,
  "spo2_raw": 97.5,
  "spo2_filtered": 98.2,
  "bpm": 82,
  "beatAvg": 80,
  "bp": "120/80",
  "sys_peak": 867,
  "dia_peak": 412,
  "pulse_area": 2310
}


Response:

{
  "status": "ok",
  "glucose_range": "NORMAL",
  "bp_source": "CUFF"
}

GET /latest_glucose

Returns last predicted glucose range.

🤖 Machine Learning Model

Model stored in:

backend/glucose_range_model.pkl


Features used:

PPG_Signal

Heart_Rate

Systolic/Diastolic Peak

Pulse Area

User constants (Age, Height, Weight, Gender)

Prediction is a glucose range class, e.g.:

LOW

NORMAL

BORDERLINE

HIGH

📈 Future Additions

Web dashboard (charts, vitals timeline, alerts)

User login system

Cloud deployment (Azure / Render)

Mobile app

Improved ML model

📝 License

This project is currently unlicensed.
You can add a LICENSE file later.

🙌 Author

Shubham Panwar
💬 ESP32 Developer • Machine Learning • IoT Systems
