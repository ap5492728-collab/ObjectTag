#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>

#define I2C_SDA 8
#define I2C_SCL 9
#define MPU_ADDR 0x68

// Unique Secret System ID for secure pair matching
#define SECRET_SYSTEM_ID "SEC_TAG_88_"

// Threshold Configuration for Anti-Shake Drop Signature
const float FREEFALL_THRESHOLD      = 6500.0;   // Sustained near 0g state in mid-air (< 0.40g)
const float IMPACT_THRESHOLD        = 26000.0;  // High-g spike when hitting the ground (> 1.6g)
const unsigned long MIN_FALL_TIME   = 120;      // Must be continuously in air for >= 120ms (drop >= 7cm)
const unsigned long MAX_FALL_TIME   = 1000;     // Maximum air-time window (ms)
const unsigned long IMPACT_WINDOW   = 50;       // Ground impact must occur within 50ms of landing

BLEAdvertising *pAdvertising;
unsigned long lastPrintTime = 0;
unsigned long alertEndTime = 0;
bool isAlertActive = false;

// State tracking
bool isFreeFalling = false;
unsigned long freeFallStartTime = 0;
bool waitingForImpact = false;
unsigned long impactWindowStart = 0;

bool initMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); // Power management register
  Wire.write(0);    // Wake up MPU6050
  return (Wire.endTransmission() == 0);
}

void setAdvertisementPayload(const char* status) {
  String fullPayload = String(SECRET_SYSTEM_ID) + String(status);
  
  BLEAdvertisementData advData;
  advData.setName("Object Tag");
  advData.setManufacturerData(fullPayload.c_str());
  pAdvertising->setAdvertisementData(advData);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); // Prevent blocking if serial monitor is disconnected

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!initMPU()) {
    if (Serial) Serial.println("MPU6050 not found!");
    while (1) { delay(100); }
  }
  if (Serial) Serial.println("MPU6050 Ready!");

  BLEDevice::init("Object Tag");
  pAdvertising = BLEDevice::getAdvertising();
  setAdvertisementPayload("IDLE");
  pAdvertising->start();

  if (Serial) Serial.println("Object Tag Active. True Drop-Only Detection Engaged.");
}

void loop() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);

  if (Wire.available() == 6) {
    int16_t ax = (Wire.read() << 8) | Wire.read();
    int16_t ay = (Wire.read() << 8) | Wire.read();
    int16_t az = (Wire.read() << 8) | Wire.read();

    float currentAccelTotal = sqrt((float)ax * ax + (float)ay * ay + (float)az * az);

    if (Serial && (millis() - lastPrintTime >= 200)) {
      Serial.print("Accel: ");
      Serial.print(currentAccelTotal);
      Serial.print(" | Falling: ");
      Serial.print(isFreeFalling ? "YES" : "NO");
      Serial.print(" | WaitImpact: ");
      Serial.println(waitingForImpact ? "YES" : "NO");
      lastPrintTime = millis();
    }

    // --- STAGE 1: CONTINUOUS FREE-FALL DETECTION ---
    if (currentAccelTotal < FREEFALL_THRESHOLD) {
      if (!isFreeFalling) {
        isFreeFalling = true;
        freeFallStartTime = millis();
        waitingForImpact = false; // Reset previous triggers
      } else {
        // Fall duration exceeded reasonable limit
        if (millis() - freeFallStartTime > MAX_FALL_TIME) {
          isFreeFalling = false;
        }
      }
    } 
    else {
      // Acceleration rose above freefall threshold
      if (isFreeFalling) {
        unsigned long fallDuration = millis() - freeFallStartTime;
        isFreeFalling = false; // Freefall ended

        // Only look for ground impact if it fell continuously for >= 120ms
        if (fallDuration >= MIN_FALL_TIME) {
          waitingForImpact = true;
          impactWindowStart = millis();
        }
        // If fallDuration < 120ms (e.g., hand shake), it is discarded immediately
      }
    }

    // --- STAGE 2: IMMEDIATE GROUND IMPACT VERIFICATION ---
    if (waitingForImpact) {
      // Ground impact spike confirmed
      if (currentAccelTotal >= IMPACT_THRESHOLD) {
        if (!isAlertActive) {
          if (Serial) {
            Serial.print("\n>>> GENUINE DROP DETECTED! Impact: ");
            Serial.print(currentAccelTotal);
            Serial.println(" >>> Broadcasting 'MOTION' Alert! <<<\n");
          }

          isAlertActive = true;
          alertEndTime = millis() + 5000;
          setAdvertisementPayload("MOTION");
        }
        waitingForImpact = false;
      } 
      // Window expired without high-g spike (e.g., smoothly caught in hand)
      else if (millis() - impactWindowStart > IMPACT_WINDOW) {
        waitingForImpact = false;
      }
    }
  }

  // Restore IDLE state after 5 seconds
  if (isAlertActive && millis() >= alertEndTime) {
    isAlertActive = false;
    setAdvertisementPayload("IDLE");
    if (Serial) Serial.println(">>> Alert cycle complete. Restoring IDLE broadcast.\n");
  }

  delay(10);