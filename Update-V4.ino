#include <WiFi.h>                  // Library untuk koneksi WiFi
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>                  // Library untuk komunikasi I2C
#include <LiquidCrystal_I2C.h>     // Library untuk mengontrol LCD I2C
#include <OneWire.h>               // Library untuk komunikasi OneWire
#include "DS18B20.h"               // Library untuk sensor suhu DS18B20
#include <Firebase_ESP_Client.h>   // Library untuk integrasi Firebase
#include "addons/TokenHelper.h"    // Library helper untuk Firebase token
#include "addons/RTDBHelper.h"     // Library helper untuk Firebase Realtime Database
#include "config-head.h"           // File header untuk konfigurasi (seperti WiFi credentials, API keys, dll)

struct WiFiConnectionManager {
  unsigned long lastReconnectAttempt = 0;
  unsigned long reconnectInterval = 10000;  // 10 detik antar percobaan reconnect
  int reconnectAttempts = 0;
  const int MAX_RECONNECT_ATTEMPTS = 5;     // Maks percobaan reconnect
  bool wasConnectedBefore = false;          // Flag koneksi sebelumnya
  unsigned long connectionLostTime = 0;     // Waktu kehilangan koneksi
} wifiManager;

// Firebase objects
FirebaseData fbdo;                 // Objek untuk menangani data Firebase
FirebaseAuth auth;                 // Objek untuk autentikasi Firebase
FirebaseConfig config;             // Objek untuk konfigurasi Firebase

// Hardware Objects
LiquidCrystal_I2C lcd(0x27, 16, 2); // Objek LCD I2C dengan alamat 0x27, 16 kolom, 2 baris
OneWire oneWire(ONE_WIRE_BUS);      // Objek OneWire untuk komunikasi dengan sensor suhu
DS18B20 tempSensor(&oneWire);       // Objek sensor suhu DS18B20

// Define constants
#define NUM_SOIL_SENSORS 5         // Number of soil moisture sensors
const int SOIL_SENSOR_PINS[NUM_SOIL_SENSORS] = {36, 39, 35, 34, 32}; // Define the pins for each sensor
const unsigned long MAX_WATERING_TIME = 2*60*1000; // Maximum watering time (2 minute in milliseconds)

// State Variables
struct SystemState {
  bool isWatering = false;          // Status penyiraman (ON/OFF)
  bool buzzerActive = false;        // Status buzzer (ON/OFF)
  unsigned long wateringStart = 0;  // Waktu mulai penyiraman
  unsigned long buzzerStart = 0;    // Waktu mulai buzzer
  int buzzerBeeps = 0;              // Jumlah beep buzzer
  unsigned long lastDataUpdate = 0; // Waktu terakhir update data
  bool manualWatering = false;      // Flag for manual watering mode
  unsigned long manualWateringDuration = 0; // Duration for manual watering in milliseconds
  unsigned long manualWateringStart = 0;    // Start time for manual watering
  bool firebaseWateringTrigger = false; // State of watering trigger from Firebase
  String displayMessage = "";      // Message to display on LCD
  unsigned long displayMessageTime = 0; // When the message was set
  bool tempConversionStarted = false; // Flag for temperature conversion
  unsigned long tempConversionStart = 0; // When temperature conversion started

} state;

bool displaySensorData = false; // Flag untuk menentukan apakah menampilkan data sensor
unsigned long lastDisplaySwitch = 0; // Waktu terakhir pergantian tampilan
const unsigned long DISPLAY_SWITCH_INTERVAL = 10000; // Durasi pergantian tampilan (10 detik)

// Cached sensor values
int cachedSoilMoisture[NUM_SOIL_SENSORS] = {0};
unsigned long lastSoilRead[NUM_SOIL_SENSORS] = {0};
float cachedTemperature = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 8*3600, 0);

void logWiFiStatus() {
  switch(WiFi.status()) {
    case WL_CONNECTED:
      Serial.println("[WiFi] Terhubung dengan sukses");
      break;
    case WL_NO_SSID_AVAIL:
      Serial.println("[WiFi] SSID tidak tersedia");
      break;
    case WL_CONNECT_FAILED:
      Serial.println("[WiFi] Koneksi gagal");
      break;
    case WL_IDLE_STATUS:
      Serial.println("[WiFi] Status idle");
      break;
    case WL_DISCONNECTED:
      Serial.println("[WiFi] Terputus");
      break;
    default:
      Serial.print("[WiFi] Status tidak dikenal: ");
      Serial.println(WiFi.status());
      break;
  }
}

void reconstructWiFiConnection() {
  unsigned long currentMillis = millis();
  
  // Periksa apakah sudah waktunya mencoba reconnect
  if (currentMillis - wifiManager.lastReconnectAttempt < wifiManager.reconnectInterval) {
    return;
  }

    wifiManager.reconnectAttempts++;
  wifiManager.lastReconnectAttempt = currentMillis;
  
  Serial.print("[WiFi] Percobaan reconnect ke-");
  Serial.println(wifiManager.reconnectAttempts);
  
  // Log status WiFi sebelum reconnect
  logWiFiStatus();
  
  // Putuskan koneksi dan bersihkan konfigurasi
  WiFi.disconnect(true);
  delay(1000);
  
  // Mulai koneksi ulang
  WiFi.mode(WIFI_STA);  // Set eksplisit ke mode stasiun
  WiFi.begin(ssid, password);
  
  // Tunggu koneksi dengan timeout
  unsigned long connectStartTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    
    // Timeout setelah 10 detik
    if (millis() - connectStartTime > 10000) {
      Serial.println("\n[WiFi] Timeout koneksi");
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Berhasil reconnect!");
    Serial.print("Alamat IP: ");
    Serial.println(WiFi.localIP());
    
    // Reset parameter reconnect
    wifiManager.reconnectAttempts = 0;
    wifiManager.reconnectInterval = 10000; // Kembalikan ke interval default
    wifiManager.wasConnectedBefore = true;
    wifiManager.connectionLostTime = 0;
    
    // Update LCD untuk menunjukkan status koneksi
    lcd.clear();
    lcd.print("WiFi Terhubung");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(2000);
    lcd.clear();
  } else {
    // Tangani kegagalan reconnect berulang
    if (wifiManager.reconnectAttempts >= wifiManager.MAX_RECONNECT_ATTEMPTS) {
      Serial.println("[WiFi] Percobaan reconnect berkali-kali gagal!");
      
      // Interval reconnect adaptif (exponential backoff)
      wifiManager.reconnectInterval *= 2; 
      
      // Jika sebelumnya pernah terkoneksi, pertimbangkan restart
      if (wifiManager.wasConnectedBefore) {
        Serial.println("[WiFi] Mencoba restart sistem...");
        
        // Update LCD untuk menunjukkan status restart
        lcd.clear();
        lcd.print("WiFi Gagal");
        lcd.setCursor(0, 1);
        lcd.print("Restart...");
        delay(2000);
        
        ESP.restart();
      }
    }
    
    // Lacak berapa lama sudah terputus
    if (wifiManager.connectionLostTime == 0) {
      wifiManager.connectionLostTime = currentMillis;
    }
  }
}
void tokenStatusCallback(firebase_auth_token_info_t info) {
    Serial.print("Token status: ");
    switch (info.status) {
        case token_status_uninitialized:
            Serial.println("Uninitialized");
            break;
        case token_status_on_request:
            Serial.println("Requesting...");
            break;
        case token_status_on_refresh:
            Serial.println("Refreshing...");
            break;
        case token_status_error:
            Serial.println("Error!");
            break;
        default:
            Serial.println("Unknown status");
            break;
    }

    // 🔹 Cek apakah Firebase sudah siap
    if (Firebase.ready()) {
        Serial.println("Firebase is READY!");
    } else {
        Serial.println("Firebase NOT ready!");
    }
}

void setup() {
  Serial.begin(115200);             // Inisialisasi serial monitor dengan baud rate 115200
  initHardware();                   // Inisialisasi hardware (pin mode, sensor, dll)
  connectWiFi();                    // Menghubungkan ke jaringan WiFi
  Serial.println("Mendapatkan waktu dari server NTP ");
  lcd.print("Mendapatkan Waktu...");

  while (timeClient.getEpochTime() < 1577836800) { // 1 Jan 2020 sebagai batas bawah
    Serial.print(".");
    timeClient.update();
    delay(1000);
  }
  Serial.println("Waktu valid diterima!");
  timeClient.begin();
  timeClient.update(); 

  String currentTime = getFormattedTime();
  String currentDate = getFormattedDate();
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(currentDate);
  lcd.setCursor(0, 1);
  lcd.print(currentTime);
  
  Serial.print("Current Date: ");
  Serial.println(currentDate);
  Serial.print("Current Time: ");
  Serial.println(currentTime);
  
  // Berikan delay 2 detik untuk melihat waktu
  delay(2000);
  
  // Initial temperature reading
  tempSensor.begin();
  tempSensor.setResolution(8);     // Set resolusi sensor suhu menjadi 12 bit

  // Tampilkan pesan kalibrasi pada LCD
  lcd.clear();
  lcd.print("Calibrating");
  lcd.setCursor(0, 1);
  lcd.print("Temperature...");
  
  Serial.println("Calibrating temperature sensor...");

  // Baca nilai suhu awal dan tampilkan pada LCD
  tempSensor.requestTemperatures();
  while (!tempSensor.isConversionComplete()) {
    delay(10);
  }
  float discardReading = tempSensor.getTempC();
  
  lcd.clear();
  lcd.print("Initial Temp:");
  lcd.setCursor(0, 1);
  lcd.print(discardReading);
  lcd.print(" C");
  
  Serial.print("Initial reading (discarded): ");
  Serial.println(discardReading);
  
  // Tunggu 3 detik untuk stabilisasi
  delay(3000);
  
  // Baca nilai suhu yang sudah stabil dan tampilkan pada LCD
  tempSensor.requestTemperatures();
  while (!tempSensor.isConversionComplete()) {
    delay(10);
  }
  cachedTemperature = tempSensor.getTempC();
  
  lcd.clear();
  lcd.print("Calibrated Temp:");
  lcd.setCursor(0, 1);
  lcd.print(cachedTemperature);
  lcd.print(" C");
  
  Serial.print("Calibrated temperature: ");
  Serial.println(cachedTemperature);
  
  // Tunggu 3 detik sebelum melanjutkan ke inisialisasi Firebase
  delay(3000);
  
  initFirebase();  
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Check watering triggers every loop
  checkWateringTriggers();

  checkRestart();
  
  // Handle buzzer state
  handleBuzzer();
  
  // Check WiFi less frequently
  static unsigned long lastWiFiCheck = 0;
  if (currentMillis - lastWiFiCheck >= 10000) {
    checkWiFi();
    lastWiFiCheck = currentMillis;
  }
  
  // Check soil moisture periodically
  static unsigned long lastSoilCheck = 0;
  if (currentMillis - lastSoilCheck >= 5000) {
    checkSoilMoisture();
    lastSoilCheck = currentMillis;
  }
  
  // Update displayupdate

  updateDisplay();
  
  // Update temperature reading periodically
  static unsigned long lastTempCheck = 0;
  if (currentMillis - lastTempCheck >= 1000) {
    updateTemperature();
    lastTempCheck = currentMillis;
  }
  
  // Update Firebase data
  static unsigned long lastFirebaseUpdate = 0;
  if (currentMillis - lastFirebaseUpdate >= 5000) {
    updateFirebaseData();
    lastFirebaseUpdate = currentMillis;
  }
  
  // Small delay to prevent CPU overload
  delay(500); // Much smaller delay than before
}

void initHardware() {
  pinMode(BUZZER_PIN, OUTPUT);      // Set pin buzzer sebagai output
  pinMode(PUMP_RELAY, OUTPUT);      // Set pin relay pompa sebagai output
  digitalWrite(PUMP_RELAY, HIGH);   // Matikan relay pompa (HIGH = OFF)
  tempSensor.begin();               // Inisialisasi sensor suhu DS18B20
  lcd.init();                       // Inisialisasi LCD
  lcd.backlight();                  // Nyalakan backlight LCD
}

void connectWiFi() {
  WiFi.begin(ssid, password);       // Mulai koneksi WiFi dengan SSID dan password
  lcd.print("Connecting WiFi");
  Serial.println("");
  Serial.println("Connecting WiFi");
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi Connected!");
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  } else {
    lcd.clear();
    lcd.print("WiFi Failed!");
    Serial.println("\nFailed to connect!");
  }
  delay(2000);
  lcd.clear();
}

void initFirebase() {
  Serial.println("Initializing Firebase...");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Init Firebase...");
  config.api_key = API_KEY;             // Set API Key Firebase
  config.database_url = DATABASE_URL;   // Set URL database Firebase
  auth.user.email = USER_EMAIL;         // Set email untuk autentikasi Firebase
  auth.user.password = USER_PASSWORD;   // Set password untuk autentikasi Firebase
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);         // Aktifkan reconnect WiFi otomatis
  
  // Set up the watering trigger node if it doesn't exist
  if (Firebase.ready()) {
    if (!Firebase.RTDB.getBool(&fbdo, "control/watering_trigger")) {
      Firebase.RTDB.setBool(&fbdo, "control/watering_trigger", false);
    }
    if (!Firebase.RTDB.getBool(&fbdo, "control/restart_trigger")) {
      Firebase.RTDB.setBool(&fbdo, "control/restart_trigger", false);
    }
    if (!Firebase.RTDB.getInt(&fbdo, "control/watering_duration")) {
      Firebase.RTDB.setInt(&fbdo, "control/watering_duration", 5); // Default 5 minutes
    }
    
    Serial.println("Firebase Connected!");
    lcd.setCursor(0, 1);
    lcd.print("Connected!");
  } else {
    Serial.println("Firebase Connection Failed!");
    lcd.setCursor(0, 1);
    lcd.print("Failed!");
  }
  delay(2000);  // Only delay briefly during initialization
  lcd.clear();
}

void checkWiFi() {
  unsigned long currentMillis = millis();
  
  // Jika tidak terkoneksi, coba rekonstruksi
  if (WiFi.status() != WL_CONNECTED) {
    // Jika ini pertama kali terdeteksi terputus
    if (wifiManager.connectionLostTime == 0) {
      wifiManager.connectionLostTime = currentMillis;
      Serial.println("[WiFi] Koneksi hilang!");
      
      // Update LCD untuk menunjukkan terputus
      lcd.clear();
      lcd.print("WiFi Terputus");
      delay(2000);
      lcd.clear();
    }
    
    // Coba reconnect
    reconstructWiFiConnection();
  } else {
    // Reset pelacakan kehilangan koneksi saat terhubung
    wifiManager.connectionLostTime = 0;
    wifiManager.reconnectAttempts = 0;
    wifiManager.reconnectInterval = 10000;
  }
}

void checkRestart() {
  // Check Firebase trigger
  if (Firebase.ready()) {
    if (Firebase.RTDB.getBool(&fbdo, "control/restart_trigger")) {
      bool restartTrigger = fbdo.boolData();
      if (restartTrigger) {
        // Set nilai menjadi false sebelum restart
        Firebase.RTDB.setBool(&fbdo, "control/restart_trigger", false);
        ESP.restart();
      }
    }
  }
}

void checkWateringTriggers() {  
  // Check Firebase trigger
  if (Firebase.ready()) {
    if (Firebase.RTDB.getBool(&fbdo, "control/watering_trigger")) {
      bool firebaseTrigger = fbdo.boolData();
      
      // If Firebase trigger changes from false to true, start watering
      if (firebaseTrigger && !state.firebaseWateringTrigger) {
        if (!state.isWatering) {
          // Get duration from Firebase (default to 5 minutes if not available)
          int durationMinutes = 5;
          if (Firebase.RTDB.getInt(&fbdo, "control/watering_duration")) {
            durationMinutes = fbdo.intData();
          }
          
          // Start watering with specified duration
          unsigned long duration = durationMinutes * 60 * 1000;
          startManualWatering(duration);
          
          // Set display message without blocking
          state.displayMessage = "Firebase trigger: " + String(durationMinutes) + "min";
          state.displayMessageTime = millis();
          
          // Log event to console
          Serial.println("Firebase trigger activated. Starting watering for " + String(durationMinutes) + " minutes");
        }
      }
      // If Firebase trigger changes from true to false, stop watering
      else if (!firebaseTrigger && state.firebaseWateringTrigger) {
        if (state.isWatering) {
          stopWatering();
          
          // Set display message without blocking
          state.displayMessage = "Firebase trigger: Watering stopped";
          state.displayMessageTime = millis();
          
          Serial.println("Firebase trigger deactivated. Stopping watering.");
        }
      }
      
      // Save current Firebase trigger state
      state.firebaseWateringTrigger = firebaseTrigger;
    }
  }
  
  // Check if manual watering should be stopped based on duration
  if (state.manualWatering && state.isWatering) {
    if (millis() - state.manualWateringStart >= state.manualWateringDuration) {
      stopWatering();
      state.manualWatering = false;
      
      // Reset Firebase trigger if it was active
      if (state.firebaseWateringTrigger && Firebase.ready()) {
        Firebase.RTDB.setBool(&fbdo, "control/watering_trigger", false);
      }
      
      // Set display message without blocking
      state.displayMessage = "Watering completed : " + String(state.manualWateringDuration / 60000) + " Min";
      state.displayMessageTime = millis();
    }
  }
}

void updateTemperature() {
  static bool conversionStarted = false;
  static unsigned long conversionStartTime = 0;
  unsigned long currentMillis = millis();
  
  if (!conversionStarted) {
    // Start temperature conversion
    tempSensor.requestTemperatures();
    conversionStarted = true;
    conversionStartTime = currentMillis;
    return;
  }
  
  // Check if conversion is complete or timed out
  if (tempSensor.isConversionComplete() || currentMillis - conversionStartTime >= 1000) {
    cachedTemperature = tempSensor.getTempC();
    conversionStarted = false;
  }
}

int readSoilSensor(int pin) {
  // Find the sensor index for this pin
  int sensorIndex = -1;
  for (int i = 0; i < NUM_SOIL_SENSORS; i++) {
    if (SOIL_SENSOR_PINS[i] == pin) {
      sensorIndex = i;
      break;
    }
  }
  
  if (sensorIndex == -1) return 0; // Invalid pin
  
  // Only read every 1 second to avoid blocking
  unsigned long currentMillis = millis();
  if (currentMillis - lastSoilRead[sensorIndex] > 1000) {
    int rawValue = analogRead(pin);
    int mappedValue = map(rawValue, DRY_VALUE, WET_VALUE, 0, 100);
    mappedValue = constrain(mappedValue, 0, 100);
    
    cachedSoilMoisture[sensorIndex] = mappedValue;
    lastSoilRead[sensorIndex] = currentMillis;
  }
  
  return cachedSoilMoisture[sensorIndex];
}

void updateDisplay() {
  static unsigned long lastUpdate = 0;
  unsigned long currentMillis = millis();
  
  // Handle special display messages first (with auto-timeout)
  if (state.displayMessage != "" && currentMillis - state.displayMessageTime < 2000) {
    // Display the message for 2 seconds
    lcd.clear();
    
    // If message is too long, split it
    if (state.displayMessage.length() > 16) {
      String line1 = state.displayMessage.substring(0, 16);
      String line2 = state.displayMessage.substring(16);
      
      lcd.setCursor(0, 0);
      lcd.print(line1);
      lcd.setCursor(0, 1);
      lcd.print(line2);
    } else {
      lcd.setCursor(0, 0);
      lcd.print(state.displayMessage);
    }
    
    return; // Skip regular display update
  } else if (state.displayMessage != "" && currentMillis - state.displayMessageTime >= 2000) {
    // Clear the message after timeout
    state.displayMessage = "";
  }
  
  // Regular display update every 2 seconds
  if (currentMillis - lastUpdate < 2000) return;
  
  // Update display using cached temperature
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(cachedTemperature,1);
  lcd.write(223);
  lcd.print("C Pump:");
  lcd.print(state.isWatering ? "ON " : "OFF");
  
  lcd.setCursor(0, 1);
  
  // Display manual watering info if active
  if (state.manualWatering && state.isWatering) {
    unsigned long remainingTime = (state.manualWateringDuration - (currentMillis - state.manualWateringStart)) / 1000;
    lcd.print("Watering: ");
    lcd.print(remainingTime);
    lcd.print("s");
  } else {
    lcd.print(WiFi.status() == WL_CONNECTED ? "WiFi Connected" : "WiFi Disconnected");
  }
  
  lastUpdate = currentMillis;
}

void checkSoilMoisture() {
  Serial.println("Checking soil moisture...");
  int dryCount = 0;
  int averageMoisture = 0;
  
  for (int i = 0; i < NUM_SOIL_SENSORS; i++) {
    int moisture = readSoilSensor(SOIL_SENSOR_PINS[i]);
    Serial.print("Sensor "); Serial.print(i + 1);
    Serial.print(" moisture: "); Serial.println(moisture);
    
    averageMoisture += moisture;
    
    if (moisture < MOISTURE_THRESHOLD) {
      dryCount++;
    }
  }
  
  averageMoisture /= NUM_SOIL_SENSORS;
  Serial.print("Average moisture: "); Serial.println(averageMoisture);
  Serial.print("Dry sensor count: "); Serial.println(dryCount);
  Serial.println("============================");
  
  // Don't start automatic watering if manual watering is active
  if (dryCount >= 2 && !state.isWatering && !state.manualWatering) {
    Serial.println("Starting watering...");
    startWatering();
  }
  
  // Stop watering if soil is sufficiently wet OR if pump has been running for too long
  unsigned long currentMillis = millis();
  if (!state.manualWatering && state.isWatering && 
      ((dryCount < 1 && averageMoisture > MOISTURE_THRESHOLD) || 
       (currentMillis - state.wateringStart >= MAX_WATERING_TIME))) {
    if (currentMillis - state.wateringStart >= MAX_WATERING_TIME) {
      Serial.println("Stopping watering (timeout)...");
    } else {
      Serial.println("Stopping watering (moisture condition)...");
    }
    stopWatering();
  }
}

void updateFirebaseData() {
  if (!Firebase.ready()) return;
  
  // Create JSON object for soil moisture data
  FirebaseJson soilMoistureJson;
  for (int i = 0; i < NUM_SOIL_SENSORS; i++) {
    String sensorKey = "sensor_" + String(i + 1);
    int moisture = readSoilSensor(SOIL_SENSOR_PINS[i]);
    soilMoistureJson.set(sensorKey, moisture);
  }
  
  unsigned long currentMillis = millis();
  
  FirebaseJson realtimeJson;
  realtimeJson.set("soil_moisture", soilMoistureJson);
  realtimeJson.set("temperature", cachedTemperature);
  realtimeJson.set("pump_status", state.isWatering);
  realtimeJson.set("is_manual_watering", state.manualWatering);
  
  // Add remaining watering time if applicable
  if (state.manualWatering && state.isWatering) {
    unsigned long remainingTime = (state.manualWateringDuration - (currentMillis - state.manualWateringStart)) / 1000;
    realtimeJson.set("remaining_watering_time", remainingTime);
  } else {
    realtimeJson.set("remaining_watering_time", 0);
  }
  
  if (Firebase.RTDB.setJSON(&fbdo, "realtime_data", &realtimeJson)) {
    Serial.println("Realtime data updated successfully");
    Serial.println("============================");
  } else {
    Serial.println("Failed to update realtime data");
    Serial.println("============================");
    Serial.println(fbdo.errorReason());
  }
}

String getFormattedTime() {
  timeClient.update(); // Perbarui waktu dari NTP server
  unsigned long epochTime = timeClient.getEpochTime(); // Dapatkan waktu dalam format Unix timestamp

  // Konversi epochTime ke struct tm
  time_t rawtime = (time_t)epochTime; // Pastikan epochTime bertipe time_t
  struct tm *timeinfo;
  timeinfo = localtime(&rawtime); // Gunakan alamat dari rawtime

  char timeStr[9];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeinfo); // Format waktu menjadi HH:MM:SS
  return String(timeStr);
}

String getFormattedDate() {
  timeClient.update(); // Perbarui waktu dari NTP server
  unsigned long epochTime = timeClient.getEpochTime(); // Dapatkan waktu dalam format Unix timestamp

  // Konversi epochTime ke struct tm
  time_t rawtime = (time_t)epochTime; // Pastikan epochTime bertipe time_t
  struct tm *timeinfo;
  timeinfo = localtime(&rawtime); // Gunakan alamat dari rawtime

  char dateStr[11];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", timeinfo); // Format tanggal menjadi YYYY-MM-DD
  return String(dateStr);
}

void beepBuzzer(int beeps) {
  state.buzzerActive = true;
  state.buzzerStart = millis();
  state.buzzerBeeps = beeps;
}

// Fungsi handleBuzzer dimodifikasi untuk beep selama 2 detik
void handleBuzzer() {
  if (!state.buzzerActive) return;
  
  unsigned long currentMillis = millis();
  unsigned long elapsed = currentMillis - state.buzzerStart;
  
  // Each beep cycle is 2500ms (2000ms on, 500ms off)
  int cycle = elapsed / 2500;
  int phase = elapsed % 2500;
  
  if (cycle >= state.buzzerBeeps) {
    // All beeps completed
    digitalWrite(BUZZER_PIN, LOW);
    state.buzzerActive = false;
    return;
  }
  
  // During the first 2000ms of each cycle, buzzer is ON
  if (phase < 2000) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void startManualWatering(unsigned long duration) {
  state.manualWatering = true;
  state.manualWateringDuration = duration;
  state.manualWateringStart = millis();
  
  // Start the watering process
  startWatering();
  
  // Record manual watering event in Firebase
  if (Firebase.ready()) {
    String currentDate = getFormattedDate();
    String currentTime = getFormattedTime();
    String historyPath = "history_data/" + currentDate + "/" + currentTime;
    
    FirebaseJson historyJson;
    historyJson.set("event", "manual_watering_start");
    historyJson.set("timestamp", currentTime);
    historyJson.set("duration_minutes", duration / 60000);
    historyJson.set("temperature", cachedTemperature);
    
    FirebaseJson soilMoistureJson;
    for (int i = 0; i < NUM_SOIL_SENSORS; i++) {
      int moisture = readSoilSensor(SOIL_SENSOR_PINS[i]);
      soilMoistureJson.set("sensor_" + String(i + 1), moisture);
    }
    historyJson.set("soil_moisture", soilMoistureJson);
    
    if (Firebase.RTDB.setJSON(&fbdo, historyPath, &historyJson)) {
      Serial.println("Manual watering event recorded successfully");
    } else {
      Serial.println("Failed to record manual watering event");
      Serial.println(fbdo.errorReason());
    }
  }
}

// Fungsi startWatering dimodifikasi untuk membunyikan buzzer sebelum penyiraman
void startWatering() {
  // Bunyikan buzzer 3 kali selama 2 detik sebelum mulai penyiraman
  beepBuzzer(3);
  
  // Tunggu sampai buzzer selesai (3 x 2.5 detik = 7.5 detik)
  unsigned long buzzerWaitStart = millis();
  while (state.buzzerActive) {
    handleBuzzer();
    // Prevent infinite loop, max wait 8 seconds
    if (millis() - buzzerWaitStart > 8000) break;
    delay(10);
  }
  
  state.isWatering = true;          // Set status penyiraman menjadi ON
  state.wateringStart = millis();   // Catat waktu mulai penyiraman
  digitalWrite(PUMP_RELAY, LOW);    // Nyalakan relay pompa (LOW = ON)
  Serial.println("Pump: ON");
  
  // Langsung kirim data ke Firebase sebagai history saat penyiraman dimulai
  if (Firebase.ready()) {
    String currentDate = getFormattedDate();
    String currentTime = getFormattedTime();
    String historyPath = "history_data/" + currentDate + "/" + currentTime;
    
    FirebaseJson historyJson;
    historyJson.set("event", "watering_start");
    historyJson.set("timestamp", currentTime);
    historyJson.set("temperature", cachedTemperature);
    
    FirebaseJson soilMoistureJson;
    for (int i = 0; i < NUM_SOIL_SENSORS; i++) {
      int moisture = readSoilSensor(SOIL_SENSOR_PINS[i]);
      soilMoistureJson.set("sensor_" + String(i + 1), moisture);
    }
    historyJson.set("soil_moisture", soilMoistureJson);
    
    if (Firebase.RTDB.setJSON(&fbdo, historyPath, &historyJson)) {
      Serial.println("Watering start event recorded successfully");
    } else {
      Serial.println("Failed to record watering start event");
      Serial.println(fbdo.errorReason());
    }
  }
}

void stopWatering() {
  state.isWatering = false;         // Set status penyiraman menjadi OFF
  digitalWrite(PUMP_RELAY, HIGH);   // Matikan relay pompa (HIGH = OFF)
  Serial.println("Pump: OFF");
}