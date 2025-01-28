#include "Arduino.h"
#include "WiFi.h"
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"
#include <LittleFS.h>
#include <FS.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// Wi-Fi podaci
const char* ssid = "your_wifi_host";
const char* password = "wifi_pw";

// Firebase podaci
#define API_KEY "xxxxxxxxxxxxxxxxxxxxx"
#define USER_EMAIL "xxxxxxxxxxxxxxxxxxxxx"
#define USER_PASSWORD "xxxxxxxxxxxxxxxxxxxxx"
#define STORAGE_BUCKET_ID "xxxxxxxxxxxxxxxxxxxxx"

// URL baze podataka
#define DATABASE_URL "xxxxxxxxxxxxxxxxxxxxx"

// PIR senzor
#define PIR_PIN 13 // Pin na koji je spojen PIR senzor

// Firebase Realtime Database reference
#define FIREBASE_CAPTURE_PHOTO_PATH "/commands/capturePhoto"

// Firebase Realtime Database reference za PIR senzor
#define FIREBASE_PIR_SENSOR_PATH "/commands/pirSensor"


WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig configF;

boolean motionDetected = false;
bool pirSensorActive = true; 

void capturePhotoAndUpload(String photoPath);
void deleteFileFromLittleFS(String photoPath);
void reinitializeCamera();
void initWiFi();
void initLittleFS();
void initCamera();
void checkFirebaseCommand();

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);

  digitalWrite(PIR_PIN, LOW);

  initWiFi();
  initLittleFS();
  // WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Onemogućavanje brownout detekcije
  initCamera();

  timeClient.begin();
  timeClient.update();

  // Firebase konfiguracija
  configF.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  configF.token_status_callback = tokenStatusCallback;
  
  // Dodaj Firebase URL ovdje
  configF.database_url = DATABASE_URL;

  Firebase.begin(&configF, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  timeClient.update();
  checkFirebaseCommand();

  // Provjera PIR senzora samo ako je aktiviran putem Firebase-a
  if (pirSensorActive && digitalRead(PIR_PIN) == HIGH) {
    if (!motionDetected) {
      Serial.println("Motion detected!");
      motionDetected = true;

      // Kreiranje naziva slike prema datumu i vremenu
      String formattedTime = timeClient.getFormattedTime();
      formattedTime.replace(":", "-");
      String photoPath = "/" + formattedTime + ".jpg";

      // Snimanje slike i slanje na Firebase
      capturePhotoAndUpload(photoPath);
    }
  } else {
    motionDetected = false;
  }

  delay(500); // Kratko kašnjenje da se izbjegnu dupli impulsi
}

void checkFirebaseCommand() {
  // Provjeri komandu za sliku (capture photo)
  if (Firebase.ready() && Firebase.RTDB.getBool(&fbdo, FIREBASE_CAPTURE_PHOTO_PATH)) {
    bool capturePhoto = fbdo.boolData();
    if (capturePhoto) {
      Serial.println("Capture photo command received from Firebase!");

      // Kreiranje naziva slike prema datumu i vremenu
      String formattedTime = timeClient.getFormattedTime();
      formattedTime.replace(":", "-");
      String photoPath = "/" + formattedTime + ".jpg";

      // Snimanje slike i slanje na Firebase
      capturePhotoAndUpload(photoPath);

      // Resetovanje vrijednosti u Firebase na false
      Firebase.RTDB.setBool(&fbdo, FIREBASE_CAPTURE_PHOTO_PATH, false);
    }
  }

  // Provjera komande za PIR senzor
  if (Firebase.ready() && Firebase.RTDB.getBool(&fbdo, FIREBASE_PIR_SENSOR_PATH)) {
    bool pirSensorStatus = fbdo.boolData();
    if (pirSensorStatus) {
      Serial.println("PIR sensor activated from Firebase!");
      pirSensorActive = true; // Aktiviraj PIR senzor
    } else {
      Serial.println("PIR sensor deactivated from Firebase!");
      pirSensorActive = false; // Deaktiviraj PIR senzor
    }
  }
}



void capturePhotoAndUpload(String photoPath) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed, reinitializing...");
    reinitializeCamera();
    return;
  }

  File file = LittleFS.open(photoPath, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file in writing mode");
    esp_camera_fb_return(fb);
    return;
  }

  file.write(fb->buf, fb->len);
  Serial.printf("Picture saved: %s - Size: %d bytes\n", photoPath.c_str(), fb->len);
  file.close();
  esp_camera_fb_return(fb);

  // Slanje slike na Firebase
  if (Firebase.ready()) {
    Serial.print("Uploading picture to Firebase...");
    if (Firebase.Storage.upload(&fbdo, STORAGE_BUCKET_ID, photoPath.c_str(), mem_storage_type_flash, photoPath.c_str(), "image/jpeg")) {
      Serial.printf("\nDownload URL: %s\n", fbdo.downloadURL().c_str());
      deleteFileFromLittleFS(photoPath); // Obriši datoteku nakon uspješnog slanja
    } else {
      Serial.printf("Upload failed: %s\n", fbdo.errorReason().c_str());
    }
  }
}

void deleteFileFromLittleFS(String photoPath) {
  if (LittleFS.exists(photoPath)) {
    LittleFS.remove(photoPath);
    Serial.printf("File deleted: %s\n", photoPath.c_str());
  } else {
    Serial.println("File does not exist");
  }
}

void reinitializeCamera() {
  esp_camera_deinit(); // Deinicijalizacija kamere
  delay(1000);
  initCamera(); // Ponovna inicijalizacija
}

void initWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("WiFi connected!");
}

void initLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("An error occurred while mounting LittleFS");
    ESP.restart();
  } else {
    Serial.println("LittleFS mounted successfully");
  }
}

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x. Restarting...\n", err);
    ESP.restart();
  } else {
    Serial.println("Camera initialized successfully!");
  }
}
