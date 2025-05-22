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

// Pin Definitions
const int BUZZER_PIN = 25;
const int PUMP_RELAY = 26;
const int ONE_WIRE_BUS = 14;


// ajust to input value soil sensor
const int DRY_VALUE = 4095;    
const int WET_VALUE = 1700;    
const int MOISTURE_THRESHOLD = 55;
#endif 
