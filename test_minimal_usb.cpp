// Minimal USBHost_t36 test - based on Mouse.ino example
#include <Arduino.h>
#include <USBHost_t36.h>

// USBHost_t36 standard pattern - exactly like Mouse.ino example
USBHost myusb;
USBHub hub1(myusb);
KeyboardController keyboard1(myusb);
MouseController mouse1(myusb);
USBHIDParser hid1(myusb);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 5000) ; // wait up to 5 seconds for Serial
    
    Serial.println("\n=== Minimal USBHost_t36 Test ===");
    Serial.println("Based on Mouse.ino example pattern");
    
    // Wait a moment before starting USB Host
    delay(1500);
    
    Serial.println("Starting USB Host...");
    myusb.begin();
    
    // Set up callbacks
    keyboard1.attachPress([](int unicode) {
        Serial.println("*** KEYBOARD KEY PRESSED! ***");
    });
    
    Serial.println("USB Host started. Plug in any USB device (mouse, keyboard, etc.)");
    Serial.println("Looking for device enumeration...");
}

void loop() {
    myusb.Task();
    
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > 2000) {
        lastStatus = millis();
        Serial.println("USB Host Task() running... (plug in a device)");
    }
} 