#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include "secrets.h"

// Definisi Pin Hardware
#define TRIG_PIN 5
#define ECHO_PIN 18
#define RAIN_PIN 34
#define LED_HIJAU 27
#define LED_KUNING 26
#define LED_MERAH 25
#define BUZZER_PIN 33
#define SERVO_PIN 13

const float TINGGI_WADAH_CM = 4.5;  // Jarak sensor ke dasar wadah saat kosong
const float BATAS_WASPADA   = 1.2;  // Naik ke WASPADA jika air >= 1.2 cm
const float BATAS_BAHAYA    = 2.0;  // Naik ke BAHAYA jika air >= 2.0 cm
const float BATAS_SURUT     = 1.1;  // Langsung lepas dari BAHAYA begitu turun <= 1.1 cm
const float DEADZONE_KOSONG = 0.7;  // Bacaan < 0.7 cm langsung dianggap 0.0 cm (Hijau)

WiFiClientSecure espClient;
PubSubClient client(espClient);
Servo pintuAir;

float lastSentTinggiAir = -999.0;
String lastSentStatus = "";
unsigned long lastSendTime = 0;
unsigned long lastReconnectAttempt = 0;

unsigned long totalPembacaanSensor = 0;
unsigned long totalPesanTerkirim = 0;

int sudutSekarang = -1;
float tinggiAirTerakhir = 0.0; // Menyimpan bacaan valid terakhir (anti-reset ke 0)
bool sedangBahaya = false;     // Mengunci status BAHAYA agar tidak kedip ke hijau

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_HIJAU, OUTPUT);
  pinMode(LED_KUNING, OUTPUT);
  pinMode(LED_MERAH, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  pintuAir.attach(SERVO_PIN);
  pintuAir.write(10);
  sudutSekarang = 10;

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  espClient.setCACert(root_ca);
  client.setServer(TB_SERVER, TB_PORT);
}

bool reconnectMQTT() {
  if (client.connect("ESP32_FloodGuard", TB_TOKEN, NULL)) {
    Serial.println("[CLOUD] Terhubung ke ThingsBoard via MQTT TLS (8883)!");
    return true;
  }
  Serial.print("[CLOUD] Gagal konek MQTT, rc=");
  Serial.println(client.state());
  return false;
}

float bacaTinggiAir() {
  float totalJarak = 0;
  int sampelValid = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long durasi = pulseIn(ECHO_PIN, HIGH, 25000);
    if (durasi > 0) {
      float jarak = durasi * 0.034 / 2.0;
      if (jarak >= 0.3 && jarak <= (TINGGI_WADAH_CM + 3.0)) {
        totalJarak += jarak;
        sampelValid++;
      }
    }
    delay(25);
  }

  // Jika pantulan gagal/terlalu dekat (< 0.3 cm), PERTAHANKAN nilai terakhir (jangan reset ke 0!)
  if (sampelValid == 0) {
    return tinggiAirTerakhir;
  }

  float rataJarak = totalJarak / sampelValid;
  float tinggiAir = TINGGI_WADAH_CM - rataJarak;

  if (tinggiAir < DEADZONE_KOSONG) {
    tinggiAir = 0.0;
  }

  tinggiAirTerakhir = tinggiAir;
  return tinggiAir;
}

void loop() {
  float tinggiAir = bacaTinggiAir();
  int nilaiHujanRaw = analogRead(RAIN_PIN);
  int intensitasHujan = map(nilaiHujanRaw, 4095, 1000, 0, 100);
  intensitasHujan = constrain(intensitasHujan, 0, 100);

  String kondisiHujan = (intensitasHujan > 30) ? "HUJAN" : "KERING";
  String statusBanjir = "AMAN";
  int targetSudut = 10;

  // LOGIKA PENGUNCI (HYSTERESIS):
  // Masuk BAHAYA jika >= 2.0 cm, dan TIDAK akan keluar dari BAHAYA sampai air turun <= 0.7 cm
  if (!sedangBahaya && tinggiAir >= BATAS_BAHAYA) {
    sedangBahaya = true;
  } else if (sedangBahaya && tinggiAir <= BATAS_SURUT) {
    sedangBahaya = false;
  }

  if (sedangBahaya) {
    statusBanjir = "BAHAYA";
    digitalWrite(LED_HIJAU, LOW);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_MERAH, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    targetSudut = 180;
  } else if (tinggiAir >= BATAS_WASPADA) {
    statusBanjir = "WASPADA";
    digitalWrite(LED_HIJAU, LOW);
    digitalWrite(LED_KUNING, HIGH);
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    targetSudut = 90;
  } else {
    statusBanjir = "AMAN";
    digitalWrite(LED_HIJAU, HIGH);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    targetSudut = 0;
  }

  if (targetSudut != sudutSekarang) {
    pintuAir.write(targetSudut);
    sudutSekarang = targetSudut;
  }

  totalPembacaanSensor++;

  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = now;
        if (reconnectMQTT()) lastReconnectAttempt = 0;
      }
    } else {
      client.loop();
    }
  }

  bool perubahanTinggi = abs(tinggiAir - lastSentTinggiAir) >= 0.5;
  bool perubahanStatus = (statusBanjir != lastSentStatus);
  bool intervalHeartbeat = (millis() - lastSendTime >= 15000);

  if (client.connected() && (perubahanTinggi || perubahanStatus || intervalHeartbeat)) {
    StaticJsonDocument<256> doc;
    doc["tinggi_air"] = round(tinggiAir * 10) / 10.0;
    doc["intensitas_hujan"] = intensitasHujan;
    doc["kondisi_hujan"] = kondisiHujan;
    doc["status"] = statusBanjir;
    doc["pintu_air"] = (statusBanjir == "BAHAYA") ? "TERBUKA_PENUH" : ((statusBanjir == "WASPADA") ? "TERBUKA_SETENGAH" : "TERTUTUP");
    doc["msg_raw"] = totalPembacaanSensor;
    doc["msg_sent"] = totalPesanTerkirim + 1;

    char payload[256];
    serializeJson(doc, payload);

    if (client.publish("v1/devices/me/telemetry", payload)) {
      totalPesanTerkirim++;
      lastSentTinggiAir = tinggiAir;
      lastSentStatus = statusBanjir;
      lastSendTime = millis();
      
      Serial.print("[TERKIRIM] ");
      Serial.println(payload);
    }
  } else {
    Serial.printf("[EDGE LOKAL] Air: %.1f cm | Hujan: %d%% | Status: %s | Efisiensi: %lu/%lu\n", 
                  tinggiAir, intensitasHujan, statusBanjir.c_str(), totalPesanTerkirim, totalPembacaanSensor);
  }

  delay(400); 
}