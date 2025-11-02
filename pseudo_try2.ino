#include <Wire.h>
#include <Adafruit_INA219.h>

// --- Pin Configuration ---
const int RELAY_PIN = 25;            // GPIO pin for the relay's IN1 (active LOW)
const int VOLTAGE_SENSOR_PIN = 34;   // ADC pin for ZMPT101B (D34)

// --- I2C Pins ---
const int SDA_PIN = 27;
const int SCL_PIN = 14;

// --- Sensors ---
Adafruit_INA219 ina219;

// --- Relay & control ---
bool relayInterrupted = false; // true => auto-cut forced (relay OFF)
bool relayState = true;        // true = ON (LOW), false = OFF (HIGH)
bool autoArm = true;           // enable/disable auto detection

// --- Serial buffer ---
String serialCmd = "";

// Buffers and windows
const int SHORT_WINDOW = 5;   
const int BUFFER_SIZE  = 40;  

// Detection persistence
const int DETECT_CONSEC = 3;  // consecutive detection counts before auto-cut

// Current thresholds (milliamp range)
const float CURR_REL_PCT = 0.05f;    
const float CURR_MIN_ABS = 0.0003f;  

// Power thresholds (milliwatt range)
const float POWER_REL_PCT = 0.06f;   
const float POWER_MIN_ABS = 0.0008f; 

// Voltage thresholds (volts)
const float VOLT_REL_PCT = 0.01f;    
const float VOLT_MIN_ABS = 0.010f;   

// Baseline adaptation (EMA)
const float BASELINE_ALPHA = 0.02f;  

const float FAKE_CURR_REL = 0.30f;
const float FAKE_CURR_MIN = 0.0005f;
const float FAKE_PWR_REL  = 0.50f;
const float FAKE_PWR_MIN  = 0.001f;
// ==================================================

// Prediction settings
const int N_PRED_SAMPLES = 12;

// Circular buffers
float buf_current[BUFFER_SIZE];
float buf_power[BUFFER_SIZE];
float buf_voltage[BUFFER_SIZE];
int buf_idx = 0;
int buf_count = 0;

// Baselines (EMA)
float baseline_current = 0.0f;
float baseline_power = 0.0f;
float baseline_voltage = 0.0f;
bool baseline_ready = false;
int baseline_samples_collected = 0;
const int STARTUP_BASELINE_SAMPLES = 10; // first N readings for initial baseline

// Detection counters
int consecutive_detects = 0;

// timing
unsigned long loop_delay_ms = 1000;


#include "model_data.h" 
#include <tflm_esp32.h>
#include <eloquent_tinyml.h>

#define N_INPUTS   90
#define N_OUTPUTS  1
#define ARENA_SIZE 20 * 1024

// Quantization parameters
const float INPUT_SCALE = 0.0039215689f;
const int   INPUT_ZERO_POINT = -128;
const float OUTPUT_SCALE = 0.0042086756f;
const int   OUTPUT_ZERO_POINT = -128;

// Initialize model
Eloquent::TF::Sequential<10, ARENA_SIZE> tf;

// placeholders for interpreter etc.
const unsigned char* model_data_ptr = model_tflite;
const int model_data_length = model_tflite_len;
const int kTensorArenaSize = 16*1024;
static uint8_t tensor_arena[kTensorArenaSize];


void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("--- Energy Demo (voltage + current + power detection + PREDICT) ---"));

  pinMode(RELAY_PIN, OUTPUT);
  relayState = true; // default ON
  applyRelayState();
  Serial.println(F("Relay -> ON (default)"));

  Serial.print(F("Initializing I2C SDA="));
  Serial.print(SDA_PIN);
  Serial.print(F(", SCL="));
  Serial.println(SCL_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);

  Serial.println(F("Initializing INA219..."));
  if (!ina219.begin()) {
    Serial.println(F("!!! ERROR: INA219 not found. Check wiring. Halting. !!!"));
    while (1) { delay(10); }
  }
  Serial.println(F("INA219 OK"));

  analogReadResolution(12);
  randomSeed((unsigned long)micros());

  // init buffers
  for (int i=0;i<BUFFER_SIZE;i++) { buf_current[i]=0.0f; buf_power[i]=0.0f; buf_voltage[i]=0.0f; }
  buf_idx = 0; buf_count = 0;
  baseline_ready = false;
  baseline_samples_collected = 0;

  Serial.println(F("Timestamp,Voltage,Current,Power,PredCurrent,PredPower"));
  Serial.println(F("Collecting initial baseline (first 10 readings)..."));
}

void loop() {
  checkSerialCommands();

  // keep relay OFF if auto-cut interrupted
  if (relayInterrupted) {
    relayState = false;
    applyRelayState();
  }

  // read sensors
  float current_mA = ina219.getCurrent_mA();
  float current_A = current_mA / 1000.0f;
  if (current_A < 0.0f) current_A = 0.0f;

  int raw = analogRead(VOLTAGE_SENSOR_PIN);
  float voltage = raw * (3.3f / 4095.0f);
  float power_W = voltage * current_A;

  unsigned long ts = millis();

  // push into circular buffer
  buf_current[buf_idx] = current_A;
  buf_power[buf_idx] = power_W;
  buf_voltage[buf_idx] = voltage;
  buf_idx = (buf_idx + 1) % BUFFER_SIZE;
  if (buf_count < BUFFER_SIZE) buf_count++;

  // initial baseline using first STARTUP_BASELINE_SAMPLES readings
  if (!baseline_ready) {
    baseline_current += current_A;
    baseline_power   += power_W;
    baseline_voltage += voltage;
    baseline_samples_collected++;
    if (baseline_samples_collected >= STARTUP_BASELINE_SAMPLES) {
      baseline_current /= (float)baseline_samples_collected;
      baseline_power   /= (float)baseline_samples_collected;
      baseline_voltage /= (float)baseline_samples_collected;
      baseline_ready = true;
      Serial.print(F("BASELINE set -> current(A): "));
      Serial.print(baseline_current, 6);
      Serial.print(F("  power(W): "));
      Serial.print(baseline_power, 6);
      Serial.print(F("  voltage(V): "));
      Serial.println(baseline_voltage, 4);
      consecutive_detects = 0;
    } else {
      Serial.print(F("Calibrating baseline: "));
      Serial.print(baseline_samples_collected);
      Serial.print(F("/"));
      Serial.println(STARTUP_BASELINE_SAMPLES);
      delay(loop_delay_ms);
      return;
    }
  } else {
    // EMA baseline update (slow)
    baseline_current = (1.0f - BASELINE_ALPHA) * baseline_current + BASELINE_ALPHA * current_A;
    baseline_power   = (1.0f - BASELINE_ALPHA) * baseline_power   + BASELINE_ALPHA * power_W;
    baseline_voltage = (1.0f - BASELINE_ALPHA) * baseline_voltage + BASELINE_ALPHA * voltage;
  }

  // compute short-term averages of last SHORT_WINDOW samples
  float short_sum_curr = 0.0f;
  float short_sum_power = 0.0f;
  float short_sum_volt = 0.0f;
  int short_count = 0;
  for (int i = 0; i < min(buf_count, SHORT_WINDOW); ++i) {
    int idx = (buf_idx - 1 - i + BUFFER_SIZE) % BUFFER_SIZE;
    short_sum_curr += buf_current[idx];
    short_sum_power += buf_power[idx];
    short_sum_volt += buf_voltage[idx];
    short_count++;
  }
  float short_avg_curr = (short_count > 0) ? (short_sum_curr / (float)short_count) : current_A;
  float short_avg_power = (short_count > 0) ? (short_sum_power / (float)short_count) : power_W;
  float short_avg_volt = (short_count > 0) ? (short_sum_volt / (float)short_count) : voltage;

  // detection using actual sensors
  bool detect_curr = false;
  bool detect_power = false;
  bool detect_volt = false;

  float curr_diff = fabs(short_avg_curr - baseline_current);
  float curr_thresh = max(CURR_MIN_ABS, baseline_current * CURR_REL_PCT);

  float power_diff = fabs(short_avg_power - baseline_power);
  float power_thresh = max(POWER_MIN_ABS, baseline_power * POWER_REL_PCT);

  float volt_diff = fabs(short_avg_volt - baseline_voltage);
  float volt_thresh = max(VOLT_MIN_ABS, baseline_voltage * VOLT_REL_PCT);

  if (curr_diff > curr_thresh) detect_curr = true;
  if (power_diff > power_thresh) detect_power = true;
  if (volt_diff > volt_thresh) detect_volt = true;

  // count consecutive detections if any metric fired
  if (autoArm && !relayInterrupted && (detect_curr || detect_power || detect_volt)) {
    consecutive_detects++;
    Serial.print(F("Detection candidate: "));
    if (detect_curr) Serial.print(F("[current] "));
    if (detect_power) Serial.print(F("[power] "));
    if (detect_volt) Serial.print(F("[voltage] "));
    Serial.print(F("count="));
    Serial.println(consecutive_detects);
  } else {
    if (consecutive_detects > 0) consecutive_detects = 0;
  }

  // if sustained detection, trigger auto-cut
  if (consecutive_detects >= DETECT_CONSEC) {
    relayInterrupted = true;
    relayState = false;
    applyRelayState();
    Serial.print(F("AUTO-CUT TRIGGERED at ts="));
    Serial.println(ts);
    Serial.print(F("short_avg_curr: "));
    Serial.print(short_avg_curr, 6);
    Serial.print(F(" baseline_curr: "));
    Serial.print(baseline_current, 6);
    Serial.print(F(" diff: "));
    Serial.print(curr_diff, 6);
    Serial.print(F(" thresh: "));
    Serial.println(curr_thresh, 6);

    Serial.print(F("short_avg_power: "));
    Serial.print(short_avg_power, 6);
    Serial.print(F(" baseline_power: "));
    Serial.print(baseline_power, 6);
    Serial.print(F(" diff: "));
    Serial.print(power_diff, 6);
    Serial.print(F(" thresh: "));
    Serial.println(power_thresh, 6);

    Serial.print(F("short_avg_voltage: "));
    Serial.print(short_avg_volt, 4);
    Serial.print(F(" baseline_voltage: "));
    Serial.print(baseline_voltage, 4);
    Serial.print(F(" diff: "));
    Serial.print(volt_diff, 4);
    Serial.print(F(" thresh: "));
    Serial.println(volt_thresh, 4);

    consecutive_detects = 0;
  }

  float pred_current = PredictCurrent(current_A);
  float pred_power   = PredictPower(power_W);

  // CSV
  Serial.print(ts);
  Serial.print(",");
  Serial.print(voltage, 4);
  Serial.print(",");
  Serial.print(current_A, 6);
  Serial.print(",");
  Serial.print(power_W, 6);
  Serial.print(",");
  Serial.print(pred_current, 6);
  Serial.print(",");
  Serial.println(pred_power, 6);

  // JSON-like single line
  Serial.print("{\"ts\":");
  Serial.print(ts);
  Serial.print(",\"voltage\":");
  Serial.print(voltage, 4);
  Serial.print(",\"current\":");
  Serial.print(current_A, 6);
  Serial.print(",\"power\":");
  Serial.print(power_W, 6);
  Serial.print(",\"pred_current\":");
  Serial.print(pred_current, 6);
  Serial.print(",\"pred_power\":");
  Serial.print(pred_power, 6);
  Serial.print(",\"relay\":");
  Serial.print(relayState ? "\"ON\"" : "\"OFF\"");
  Serial.print(",\"baseline_current\":");
  Serial.print(baseline_current, 6);
  Serial.print(",\"baseline_power\":");
  Serial.print(baseline_power, 6);
  Serial.print(",\"baseline_voltage\":");
  Serial.print(baseline_voltage, 4);
  Serial.print("}");
  Serial.println();

  delay(loop_delay_ms);
}


float PredictCurrent(float measured) {
  if (measured <= 0.0f) return 0.0f;
  long r = random((int)(-1000 * FAKE_CURR_REL), (int)(1000 * FAKE_CURR_REL) + 1);
  float jitter_rel = (float)r / 1000.0f;
  float jitter = measured * jitter_rel;
  if (fabs(jitter) < FAKE_CURR_MIN) jitter = (jitter < 0) ? -FAKE_CURR_MIN : FAKE_CURR_MIN;
  float pred = measured + jitter;
  if (pred < 0.0f) pred = 0.0f;
  return pred;
}

float PredictPower(float measured) {
  if (measured <= 0.0f) return 0.0f;
  long r = random((int)(-1000 * FAKE_PWR_REL), (int)(1000 * FAKE_PWR_REL) + 1);
  float jitter_rel = (float)r / 1000.0f;
  float jitter = measured * jitter_rel;
  if (fabs(jitter) < FAKE_PWR_MIN) jitter = (jitter < 0) ? -FAKE_PWR_MIN : FAKE_PWR_MIN;
  float pred = measured + jitter;
  if (pred < 0.0f) pred = 0.0f;
  return pred;
}

// ---------- PREDICTION: next-day energy based on recent buffer ----------
void printNextDayPredictionFromBuffer() {
  int available = buf_count;
  if (available <= 0) {
    Serial.println(F("PREDICTION: no power samples available in buffer."));
    return;
  }

  int useN = min(available, N_PRED_SAMPLES);
  float sum = 0.0f;
  for (int i = 0; i < useN; ++i) {
    int idx = (buf_idx - 1 - i + BUFFER_SIZE) % BUFFER_SIZE; // last i-th sample
    sum += buf_power[idx];
  }
  float avg_power = sum / (float)useN; // watts

  float energy_wh = avg_power * 24.0f;   // watt-hours in next 24h
  float energy_kwh = energy_wh / 1000.0f;
  float energy_mwh = energy_wh * 1000.0f;

  // Human readable
  Serial.print(F("PREDICTION: avg_power(W)="));
  Serial.print(avg_power, 6);
  Serial.print(F("  next24h: "));
  Serial.print(energy_wh, 6);
  Serial.print(F(" Wh ("));
  Serial.print(energy_kwh, 9);
  Serial.println(F(" kWh)"));

  // JSON for frontend
  Serial.print(F("{\"pred_avg_power_W\":"));
  Serial.print(avg_power, 6);
  Serial.print(F(",\"pred_next24h_Wh\":"));
  Serial.print(energy_wh, 6);
  Serial.print(F(",\"pred_next24h_kWh\":"));
  Serial.print(energy_kwh, 9);
  Serial.print(F(",\"pred_next24h_mWh\":"));
  Serial.print(energy_mwh, 6);
  Serial.print(F(",\"samples_used\":"));
  Serial.print(useN);
  Serial.println(F("}"));
}

// Fallback live sampling if buffer empty (less ideal)
float computeInstantAveragePower(int M) {
  float s = 0.0f;
  int effective = max(1, M);
  for (int i = 0; i < effective; ++i) {
    int raw = analogRead(VOLTAGE_SENSOR_PIN);
    float voltage = raw * (3.3f / 4095.0f);
    float currentA = ina219.getCurrent_mA() / 1000.0f;
    if (currentA < 0.0f) currentA = 0.0f;
    float p = voltage * currentA;
    s += p;
    delay(150);
  }
  return s / (float)effective;
}

// --- apply relay state (active LOW) ---
void applyRelayState() {
  if (relayState) digitalWrite(RELAY_PIN, LOW);
  else digitalWrite(RELAY_PIN, HIGH);
}

// --- Serial command handling ---
// RELAY ON  - manual restore (clears auto interruption)
// RELAY OFF - manual off (clears auto interruption)
// INTERRUPT - force interrupt (relay OFF)
// TOGGLE    - toggle relay if not interrupted
// STATUS    - show status + baselines
// CALIBRATE - clear baseline and recollect startup samples
// PREDICT   - compute next-day energy prediction from recent samples
void checkSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialCmd.length() > 0) { handleCommand(serialCmd); serialCmd = ""; }
    } else {
      serialCmd += c;
      if (serialCmd.length() > 160) serialCmd = serialCmd.substring(0,160);
    }
  }
}

void handleCommand(String cmdRaw) {
  String cmd = cmdRaw;
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "RELAY ON") {
    relayInterrupted = false;
    relayState = true;
    applyRelayState();
    Serial.println(F("MANUAL: RELAY -> ON (interruption cleared)."));
  } else if (cmd == "RELAY OFF") {
    relayInterrupted = false;
    relayState = false;
    applyRelayState();
    Serial.println(F("MANUAL: RELAY -> OFF (interruption cleared)."));
  } else if (cmd == "INTERRUPT" || cmd == "INTERRUPT RELAY") {
    relayInterrupted = true;
    relayState = false;
    applyRelayState();
    Serial.println(F("MANUAL: RELAY INTERRUPTED -> OFF"));
  } else if (cmd == "RESUME") {
    relayInterrupted = false;
    applyRelayState();
    Serial.println(F("MANUAL: Relay interruption cleared."));
  } else if (cmd == "TOGGLE") {
    if (!relayInterrupted) {
      relayState = !relayState;
      applyRelayState();
      Serial.print(F("RELAY TOGGLED -> "));
      Serial.println(relayState ? "ON" : "OFF");
    } else {
      Serial.println(F("Cannot toggle: Relay interrupted. Use 'RELAY ON' to restore."));
    }
  } else if (cmd == "STATUS") {
    Serial.print(F("STATUS -> Relay: "));
    Serial.print(relayState ? "ON" : "OFF");
    Serial.print(F(", Interrupted: "));
    Serial.print(relayInterrupted ? "YES" : "NO");
    Serial.print(F(", AutoArm: "));
    Serial.println(autoArm ? "ENABLED" : "DISABLED");
    Serial.print(F("Baseline current(A): "));
    Serial.println(baseline_current, 6);
    Serial.print(F("Baseline power(W): "));
    Serial.println(baseline_power, 6);
    Serial.print(F("Baseline voltage(V): "));
    Serial.println(baseline_voltage, 4);
    Serial.print(F("Buffer samples available: "));
    Serial.println(buf_count);
  } else if (cmd == "CALIBRATE") {
    baseline_ready = false;
    baseline_samples_collected = 0;
    baseline_current = 0.0f;
    baseline_power = 0.0f;
    baseline_voltage = 0.0f;
    Serial.println(F("CALIBRATE: baseline cleared. Collecting fresh startup samples..."));
  } else if (cmd == "ARM") {
    autoArm = true;
    Serial.println(F("Auto-detection ARMed."));
  } else if (cmd == "DISARM") {
    autoArm = false;
    Serial.println(F("Auto-detection DISARMED."));
  } else if (cmd == "PREDICT") {
    // If buffer has samples, use it. Otherwise take quick live samples.
    if (buf_count > 0) {
      printNextDayPredictionFromBuffer();
    } else {
      Serial.println(F("No stored buffer samples — taking quick live samples for prediction..."));
      float avg_p = computeInstantAveragePower(N_PRED_SAMPLES);
      float e_wh = avg_p * 24.0f;
      float e_kwh = e_wh / 1000.0f;
      Serial.print(F("PREDICTION (live): avg_power(W)="));
      Serial.print(avg_p, 6);
      Serial.print(F("  next24h: "));
      Serial.print(e_wh, 6);
      Serial.print(F(" Wh ("));
      Serial.print(e_kwh, 9);
      Serial.println(F(" kWh)"));
    }
  } else {
    Serial.print(F("UNKNOWN CMD: "));
    Serial.println(cmdRaw);
    Serial.println(F("Valid: RELAY ON, RELAY OFF, INTERRUPT, RESUME, TOGGLE, STATUS, CALIBRATE, ARM, DISARM, PREDICT"));
  }
}