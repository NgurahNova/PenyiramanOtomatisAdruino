#ifndef CONFIG_HEAD_H
#define CONFIG_HEAD_H


// Firebase configuration
#define API_KEY "AIzaSyBvQ2rdPjw2pnu5mr-vY5vgdqx25K2kU4M"
#define DATABASE_URL "https://penyiraman-otomatis-19665-default-rtdb.asia-southeast1.firebasedatabase.app"

#define USER_EMAIL "test@mail.com"
#define USER_PASSWORD "test123"
// Konfigurasi WiFi
const char* ssid = "Sandira";
const char* password = "sandiraagro123";

// NTP Server for time synchronization
// const char* ntpServer1 = "id.pool.ntp.org";
// const char* ntpServer2 = "pool.ntp.org";
// const char* ntpServer3 = "time.google.com";
// const long gmtOffset_sec = 8*3600;  // GMT+7 for Jakarta
// const int daylightOffset_sec = 0;


// Pin Definitions
const int BUZZER_PIN = 25;
const int PUMP_RELAY = 26;
const int ONE_WIRE_BUS = 14;

// Kalibrasi Sensor
// ajust to input value soil sensor
const int DRY_VALUE = 4095;    
const int WET_VALUE = 1700;    
const int MOISTURE_THRESHOLD = 55;
#endif // CONFIG_HEAD_H
