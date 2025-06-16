#include <Arduino.h>
#include <RadioLib.h>
#include <LoRaWAN_ESP32.h>
#include <heltec_unofficial.h>

// LoRaWAN credentials - replace with your actual values
const uint64_t joinEUI = 0x0000000000000000;  // Replace with your JoinEUI
const uint64_t devEUI = 0x70B3D57ED0066298;   // Replace with your DevEUI  
const uint8_t appKey[] = {0x4E, 0xD1, 0xAB, 0x3E, 0xA4, 0x09, 0xE1, 0x32, 
                          0xA7, 0xA5, 0x37, 0x19, 0x86, 0x04, 0xAE, 0xB0}; // Replace with your AppKey
const uint8_t nwkKey[] = {0x83, 0xA6, 0x93, 0x93, 0xB3, 0xD2, 0xBB, 0xB6, 
                          0x43, 0x07, 0xB2, 0x60, 0xEB, 0x6B, 0xA1, 0xEB}; // Replace with your NwkKey

// Define which Heltec board we're using
#define HELTEC_WIRELESS_STICK

// Global variables
LoRaWANNode* node = nullptr;
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 60000; // 60 seconds (1 minute)

// RTC variables that survive deep sleep
RTC_DATA_ATTR uint8_t messageCounter = 0;
RTC_DATA_ATTR bool hasJoined = false;

// Function declarations
void sendLoRaWANMessage();
void goToDeepSleep(uint32_t sleepTimeMs);

void setup() {
  // Initialize the Heltec board (radio, display, serial)
  heltec_setup();
  
  Serial.println("\n=== LoRaWAN ESP32 with DevNonce Persistence ===");
  Serial.println("Using LoRaWAN_ESP32 for session/DevNonce management");
  
  // Check if we need to provision the device
  if (!persist.isProvisioned()) {
    Serial.println("Device not provisioned. Storing credentials...");
    
    // Provision the device with our credentials
    bool provisionResult = persist.provision(
      "EU868",    // LoRaWAN band
      0,          // sub-band (0 for EU868)
      joinEUI,
      devEUI,
      appKey,
      nwkKey
    );
    
    if (provisionResult) {
      Serial.println("Device provisioned successfully!");
    } else {
      Serial.println("ERROR: Failed to provision device!");
      return;
    }
  } else {
    Serial.println("Device already provisioned.");
  }
  
  // Create and manage the LoRaWAN node
  // This will automatically handle DevNonce persistence and session restoration
  Serial.println("Creating LoRaWAN node...");
  node = persist.manage(&radio, true); // autoJoin = true
  
  if (node == nullptr) {
    Serial.println("ERROR: Failed to create LoRaWAN node!");
    return;
  }
  
  // Check if we successfully joined
  if (node->isActivated()) {
    Serial.println("✓ LoRaWAN node joined successfully!");
    hasJoined = true;
    
    // Set duty cycle to comply with TTN Fair Use Policy
    node->setDutyCycle(true, 1250);
    
    // Print some information
    Serial.print("DevEUI: ");
    uint64_t devEuiValue = persist.getDevEUI();
    for (int i = 7; i >= 0; i--) {
      Serial.printf("%02X", (uint8_t)(devEuiValue >> (i * 8)));
    }
    Serial.println();
    
    Serial.print("Band: ");
    Serial.println(persist.getBand());
    
  } else {
    Serial.println("✗ Failed to join LoRaWAN network");
    hasJoined = false;
  }
  
  lastSendTime = millis();
}

void loop() {
  heltec_loop(); // Handle button and other Heltec functionality
  
  // Only proceed if we have a valid node and have joined
  if (node == nullptr || !hasJoined) {
    delay(1000);
    return;
  }
  
  // Check if it's time to send a message
  if (millis() - lastSendTime >= sendInterval) {
    sendLoRaWANMessage();
    lastSendTime = millis();
  }
  
  delay(100);
}

void sendLoRaWANMessage() {
  Serial.println("\n--- Sending LoRaWAN Message ---");
  
  // Prepare the message "ABCD1234" + counter
  String message = "ABCD1234_" + String(messageCounter);
  uint8_t uplinkData[20];
  message.getBytes(uplinkData, sizeof(uplinkData));
  size_t messageLength = message.length();
  
  Serial.print("Message: ");
  Serial.println(message);
  Serial.print("Counter: ");
  Serial.println(messageCounter);
  
  // Prepare downlink buffer
  uint8_t downlinkData[256];
  size_t downlinkLength = sizeof(downlinkData);
  
  // Send the message
  int16_t state = node->sendReceive(uplinkData, messageLength, 1, downlinkData, &downlinkLength);
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("✓ Message sent successfully!");
    messageCounter++;
    
    if (downlinkLength > 0) {
      Serial.print("Downlink received (");
      Serial.print(downlinkLength);
      Serial.print(" bytes): ");
      for (size_t i = 0; i < downlinkLength; i++) {
        Serial.printf("%02X ", downlinkData[i]);
      }
      Serial.println();
    } else {
      Serial.println("No downlink received.");
    }
    
  } else if (state > 0) {
    Serial.println("✓ Message sent, downlink with error code received");
    messageCounter++;
    
  } else {
    Serial.print("✗ Send failed with error: ");
    Serial.println(state);
    
    // Handle specific error cases
    switch (state) {
      case RADIOLIB_ERR_NETWORK_NOT_JOINED:
        Serial.println("Network not joined - attempting rejoin...");
        hasJoined = false;
        // The persist library will handle rejoining automatically
        break;
      case RADIOLIB_ERR_UPLINK_UNAVAILABLE:
        Serial.println("Duty cycle violation - waiting...");
        break;
      default:
        Serial.println("Unknown error occurred");
        break;
    }
  }
  
  // Save session after each transmission attempt
  // This ensures DevNonce and session state are persisted
  if (persist.saveSession(node)) {
    Serial.println("Session state saved successfully");
  } else {
    Serial.println("Warning: Failed to save session state");
  }
  
  // Calculate and display time until next allowed transmission
  uint32_t timeUntilUplink = node->timeUntilUplink();
  if (timeUntilUplink > 0) {
    Serial.print("Duty cycle: Next uplink allowed in ");
    Serial.print(timeUntilUplink / 1000);
    Serial.println(" seconds");
  }
}

// Optional: Add deep sleep functionality for battery-powered operation
void goToDeepSleep(uint32_t sleepTimeMs) {
  Serial.println("Saving session and entering deep sleep...");
  
  // Save the current session state before sleep
  persist.saveSession(node);
  
  // Configure wake-up timer
  esp_sleep_enable_timer_wakeup(sleepTimeMs * 1000); // Convert to microseconds
  
  // Enter deep sleep
  esp_deep_sleep_start();
}