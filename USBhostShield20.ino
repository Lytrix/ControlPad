#include <SPI.h>
#include <Usb.h>
#include <usbhub.h>
#include <hidboot.h>
#include "CMControlPad.h"


USB Usb;
CMControlPad myPad(&Usb);

uint8_t usbstate;
uint8_t laststate;
USB_DEVICE_DESCRIPTOR devDesc;
uint32_t lastErrorTime = 0;
uint8_t consecutiveErrors = 0;
uint32_t lastTransferTime = 0;
uint32_t lastPollTime = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ; // Wait for serial port to connect
  }
  Serial.println("\n\n🔌 USB Host starting...");
  
  // Initialize SPI
  Serial.println("📡 Initializing SPI...");
  SPI.begin();
  pinMode(SS, OUTPUT);
  digitalWrite(SS, HIGH);
  
  // Check MAX3421E revision
  Serial.println("🔍 Checking MAX3421E revision...");
  Usb.Init(); // Initializes SPI
  uint8_t revision = Usb.regRd(rREVISION);
  Serial.print("📌 MAX3421E revision: 0x");
  Serial.println(revision, HEX);
  
  if (revision == 0) {
    Serial.println("❌ MAX3421E not responding - check connections!");
    while(1); // Halt if no response
  }
  
  // Initialize USB Host
  Serial.println("📡 Initializing USB Host...");
  if (Usb.Init() == -1) {
    Serial.println("❌ USB Host Init Failed");
    while(1);
  }
  Serial.println("✅ USB Host OK");
  
  // Print VID/PID we're looking for
  Serial.print("🔍 Looking for device with VID: 0x");
  Serial.print(CM_VID, HEX);
  Serial.print(" PID: 0x");
  Serial.println(CM_PID, HEX);
  
  // Register our device
  Serial.println("📝 Registering CMControlPad device...");
  uint8_t rcode = Usb.RegisterDeviceClass(&myPad);
  if (rcode) {
    Serial.print("❌ Failed to register CMControlPad device! Error code: 0x");
    Serial.println(rcode, HEX);
    while(1);
  }
  Serial.println("✅ CMControlPad device registered");
  
  laststate = 0;
}

void loop() {
  static unsigned long lastErrorTime = 0;
  static int consecutiveErrors = 0;
  static bool deviceFound = false;
  static uint8_t foundMessages = 0;
  
  Usb.Task();
  usbstate = Usb.getUsbTaskState();
  
  if (usbstate != laststate) {
    laststate = usbstate;
    uint8_t rcode;
    
    switch(usbstate) {
      case USB_DETACHED_SUBSTATE_WAIT_FOR_DEVICE:
        Serial.println("📡 Waiting for device...");
        break;
      case USB_ATTACHED_SUBSTATE_RESET_DEVICE:
        Serial.println("📡 Device connected. Resetting...");
        break;
      case USB_ATTACHED_SUBSTATE_WAIT_SOF:
        Serial.println("📡 Reset complete. Waiting for the first SOF...");
        break;
      case USB_ATTACHED_SUBSTATE_GET_DEVICE_DESCRIPTOR_SIZE:
        Serial.println("📡 SOF generation started. Enumerating device...");
        break;
      case USB_STATE_ADDRESSING:
        Serial.println("📡 Setting device address...");
        break;
      case USB_STATE_RUNNING:
        Serial.println("📡 Device is running!");
        // Read device descriptor
        rcode = Usb.getDevDescr(1, 0, sizeof(USB_DEVICE_DESCRIPTOR), (uint8_t*)&devDesc);
        if (rcode) {
          Serial.print("❌ Error reading device descriptor. Error code: 0x");
          Serial.println(rcode, HEX);
        } else {
          Serial.println("\n📋 Device Descriptor:");
          Serial.print("  Vendor ID: 0x");
          Serial.println(devDesc.idVendor, HEX);
          Serial.print("  Product ID: 0x");
          Serial.println(devDesc.idProduct, HEX);
          Serial.print("  Device Class: 0x");
          Serial.println(devDesc.bDeviceClass, HEX);
          Serial.print("  Device Subclass: 0x");
          Serial.println(devDesc.bDeviceSubClass, HEX);
          Serial.print("  Device Protocol: 0x");
          Serial.println(devDesc.bDeviceProtocol, HEX);
          Serial.print("  Max Packet Size: ");
          Serial.println(devDesc.bMaxPacketSize0);
          
          // Check if this is our device
          if (devDesc.idVendor == CM_VID && devDesc.idProduct == CM_PID) {
            Serial.println("🎯 Found matching CMControlPad device!");
          } else {
            Serial.println("⚠️ Device VID/PID doesn't match CMControlPad");
          }
        }
        break;
      case USB_STATE_ERROR:
        Serial.println("❌ USB state machine reached error state");
        break;
    }
  }

  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 1000) {  // Print status every second
    lastCheck = millis();
    Serial.print("📊 Status - Address: 0x");
    Serial.print(myPad.getAddress(), HEX);
    Serial.print(" Initialized: ");
    Serial.println(myPad.isInitialized() ? "Yes" : "No");
  }

  if (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
    if (!deviceFound) {
      foundMessages++;
      if (foundMessages <= 3) {
        Serial.println("🎮 Found ControlPad, initializing...");
      } else if (foundMessages == 4) {
        Serial.println("🎮 ControlPad found (suppressing further messages)");
      }
      deviceFound = true;
    }

    if (myPad.isInitialized()) {
      consecutiveErrors = 0;
      // Only try transfers if we haven't seen too many errors and enough time has passed
      if (consecutiveErrors < 3 && (millis() - lastTransferTime >= 10)) {
    uint8_t buf[64];
    uint16_t len = sizeof(buf);
        const EpInfo* epInfo = myPad.getEpInfo();
        
        // Print endpoint info before transfer
        Serial.print("📡 Attempting transfer on endpoint 0x");
        Serial.print(epInfo[1].epAddr, HEX);
        Serial.print(" (max packet size: ");
        Serial.print(epInfo[1].maxPktSize);
        Serial.println(")");
        
        // Try to read from the interrupt endpoint
        uint8_t rcode = Usb.inTransfer(myPad.getAddress(), epInfo[1].epAddr, &len, buf);
        lastTransferTime = millis();
        
    if (rcode == 0 && len > 0) {
          // Reset error counter on successful transfer
          consecutiveErrors = 0;
      Serial.print("🔹 Pad Data: ");
      for (uint8_t i = 0; i < len; i++) {
        Serial.print(buf[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
        } else if (rcode != 0) {
          consecutiveErrors++;
          if (consecutiveErrors >= 3) {
            Serial.println("❌ Too many transfer errors, stopping transfer attempts");
            // Reset error counter after a longer delay
            if (millis() - lastErrorTime > 5000) {
              consecutiveErrors = 0;
              lastErrorTime = millis();
              Serial.println("🔄 Resetting error counter, will try again");
            }
          } else {
            Serial.print("❌ Transfer error: 0x");
            Serial.print(rcode, HEX);
            Serial.print(" (len: ");
            Serial.print(len);
            Serial.println(")");
          }
        }
      }
    } else {
      // Track initialization errors with reduced frequency
      unsigned long currentTime = millis();
      if (currentTime - lastErrorTime > 5000) { // Only print error every 5 seconds
        Serial.println("⚠️ Device not initialized");
        lastErrorTime = currentTime;
      }
    }
  } else {
    deviceFound = false;
    foundMessages = 0; // Reset counter when device is disconnected
    
    // Only print USB state errors occasionally
    unsigned long currentTime = millis();
    if (currentTime - lastErrorTime > 10000) { // Only print every 10 seconds
      Serial.print("🔌 USB State: ");
      Serial.println(Usb.getUsbTaskState());
      lastErrorTime = currentTime;
    }
  }
}
