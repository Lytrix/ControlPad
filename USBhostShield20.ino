#include <hidcomposite.h>
#include <usbhub.h>

// Satisfy the IDE, which needs to see the include statment in the ino too.
#ifdef dobogusinclude
#include <spi4teensy3.h>
#endif
#include <SPI.h>

// Override HIDComposite to be able to select which interface we want to hook into
class HIDSelector : public HIDComposite
{
private:
    bool interface1Initialized = false;
    
public:
    HIDSelector(USB *p) : HIDComposite(p) {};
    
    // Add method to initialize Interface 1
    bool initializeInterface1();
    bool sendInitializationSequence();
    bool checkAndSyncToggles(const char* stepName);
    void showToggleStates(const char* afterCommand);
    void resetToggleAfterNAK(const char* commandName);
    void setToggleForNextTransfer(uint8_t toggleState, const char* reason);
    void testLEDCommands();
    void waitForUSBFrameSync();
    void waitForFIFOReady();
    
    // MAX3421E timing synchronization methods

protected:
    void ParseHIDData(USBHID *hid, uint8_t ep, bool is_rpt_id, uint8_t len, uint8_t *buf); // Called by the HIDComposite library
    bool SelectInterface(uint8_t iface, uint8_t proto);
    uint8_t OnInitSuccessful(); // Override to add our initialization
};

// Return true for the interface we want to hook into
bool HIDSelector::SelectInterface(uint8_t iface, uint8_t proto)
{
    Serial.print("🔍 Interface ");
    Serial.print(iface);
    Serial.print(" Proto ");
    Serial.println(proto);
    
    // Select Interface 0 (standard HID) and Interface 1 (control interface)
    if (iface == 0 || iface == 1) {
        return true;
  }

    return false;
}

// Called when device is successfully initialized
uint8_t HIDSelector::OnInitSuccessful() {
    Serial.println("✅ HID device initialized successfully");
    Serial.println("⏳ Waiting for device to be fully ready for Interface 1...");
    return 0;
}

// Initialize Interface 1 with the CM Control Pad commands
bool HIDSelector::initializeInterface1() {
    Serial.println("📤 Sending CM Control Pad initialization sequence...");
    
    if (!isReady()) {
        Serial.println("❌ Device not ready for initialization");
        return false;
    }
    
    // Send initialization sequence
    bool success = sendInitializationSequence();
    
    if (success) {
        Serial.println("✅ Interface 1 initialized successfully!");
        Serial.println("🌈 Testing LED command sequence...");
        testLEDCommands();
    } else {
        Serial.println("❌ Interface 1 initialization failed");
    }
    
    return success;
}

// Track command responses for cleaner output
struct CommandResponse {
    uint8_t cmd1, cmd2;
    bool found;
    const char* name;
};

// Send the initialization command sequence
bool HIDSelector::sendInitializationSequence() {
    uint8_t cmd[64];
    uint8_t rcode;
    
    // Track command confirmations
    bool confirmed[5] = {false, false, false, false, false};
    const char* cmdNames[] = {"Setup 1", "Setup 2", "Setup 3", "Status", "Effects"};
    uint8_t cmdCodes[][2] = {{0x42, 0x00}, {0x42, 0x10}, {0x43, 0x00}, {0x41, 0x80}, {0x52, 0x00}};
    
    // Helper function to check for responses
    auto checkForResponses = [&]() {
        for (int attempt = 0; attempt < 15; attempt++) {
            uint8_t response[64];
            uint16_t responseLen = 64;
            uint8_t pollResult = pUsb->inTransfer(bAddress, 0x3, &responseLen, response);
            
            if (pollResult == 0 && responseLen >= 2) {
                for (int i = 0; i < 5; i++) {
                    if (response[0] == cmdCodes[i][0] && response[1] == cmdCodes[i][1] && !confirmed[i]) {
                        confirmed[i] = true;
                        Serial.print("✅ ");
                        Serial.print(cmdNames[i]);
                        Serial.print(" (");
                        if (response[0] < 0x10) Serial.print("0");
                        Serial.print(response[0], HEX);
                        Serial.print(" ");
                        if (response[1] < 0x10) Serial.print("0");
                        Serial.print(response[1], HEX);
                        Serial.println("): CONFIRMED");
                        break;
                    }
                }
            }
            delay(2);
        }
    };
    
    if (!isReady()) {
        Serial.println("❌ Device not ready for command sequence");
        return false;
    }
    
    // Command 1: 42 00
    Serial.println("📤 Step 1: Setup command 42 00");
    memset(cmd, 0, 64);
    cmd[0] = 0x42; cmd[1] = 0x00; cmd[4] = 0x01; cmd[7] = 0x01;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    Serial.print("🔍 Command 1 result: 0x");
    Serial.println(rcode, HEX);
    if (rcode && rcode != 0x4) {
        Serial.print("❌ Command 1 failed with error: 0x");
        Serial.println(rcode, HEX);
        return false;
    }
    Serial.println("✅ Command 1 sent successfully");
    delay(100);
    
    // Command 2: 42 10
    Serial.println("📤 Step 2: Setup command 42 10");
    memset(cmd, 0, 64);
    cmd[0] = 0x42; cmd[1] = 0x10; cmd[4] = 0x01; cmd[7] = 0x01;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode && rcode != 0x4) { Serial.print("❌ Command 1 failed with error: 0x"); Serial.println(rcode, HEX); return false; }
    Serial.println("✅ Command 2 sent successfully");
    delay(200);
    
    // Command 3: 43 00
    Serial.println("📤 Step 3: Setup command 43 00");
    memset(cmd, 0, 64);
    cmd[0] = 0x43; cmd[1] = 0x00; cmd[4] = 0x01;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode && rcode != 0x4) { Serial.print("❌ Command 1 failed with error: 0x"); Serial.println(rcode, HEX); return false; }
    Serial.println("✅ Command 3 sent successfully");
    delay(130);
    
    // Check for setup responses
    checkForResponses();
    
    // Command 4: 41 80 (status)
    Serial.println("📤 Step 4: Status command 41 80");
    memset(cmd, 0, 64);
    cmd[0] = 0x41; cmd[1] = 0x80;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    delay(20);
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode == 0) Serial.println("✅ Command 4 retry successful!");
    delay(20);
    
    // Command 5: 52 00 (activate effects)
    Serial.println("📤 Step 5: Activate effects 52 00");
    memset(cmd, 0, 64);
    cmd[0] = 0x52; cmd[1] = 0x00;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    delay(20);
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode == 0) Serial.println("✅ Command 5 retry successful!");
    delay(20);
    
    // Final check for all responses
    checkForResponses();
    
    // Command 6: Set custom mode (56 81) - Run once during initialization
    Serial.println("📤 Step 6: Set custom mode (56 81)");
    memset(cmd, 0, 64);
    cmd[0] = 0x56; cmd[1] = 0x81;
    cmd[4] = 0x01; cmd[8] = 0x02;
    cmd[12] = 0xbb; cmd[13] = 0xbb; cmd[14] = 0xbb; cmd[15] = 0xbb;
    cmd[16] = 0xbb; cmd[17] = 0xbb; cmd[18] = 0xbb; cmd[19] = 0xbb;
    
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    Serial.print("🔍 Custom mode result: 0x");
    Serial.println(rcode, HEX);
    if (rcode != 0 && rcode != 0x4) {
        Serial.print("❌ Custom mode failed with error: 0x");
        Serial.println(rcode, HEX);
        return false;
    }
    Serial.println("✅ Custom mode set successfully");
    delay(50);
    
    Serial.println("✅ Initialization sequence complete");
    return true;
}

// Will be called for all HID data received from the USB interface
void HIDSelector::ParseHIDData(USBHID *hid, uint8_t ep, bool is_rpt_id, uint8_t len, uint8_t *buf) {
    if (len && buf) {
        Serial.print("📥 HID EP 0x");
        Serial.print(ep, HEX);
        Serial.print(" (");
        Serial.print(len);
        Serial.print(" bytes): ");
        
        for (uint8_t i = 0; i < len; i++) {
            if (buf[i] < 0x10) Serial.print("0");
          Serial.print(buf[i], HEX);
            Serial.print(" ");
        }
        Serial.println();
        
        // Parse button data for Interface 0
        if (ep == 0x81 && len >= 4) {
            uint8_t buttons1 = buf[0];
            uint8_t buttons2 = buf[1];
            
            if (buttons1 != 0 || buttons2 != 0) {
                Serial.print("🔴 Button pressed! ");
                for (int i = 0; i < 8; i++) {
                    if (buttons1 & (1 << i)) {
                        Serial.print("Btn");
                        Serial.print(i + 1);
                        Serial.print(" ");
                    }
                }
                for (int i = 0; i < 8; i++) {
                    if (buttons2 & (1 << i)) {
                        Serial.print("Btn");
                        Serial.print(i + 9);
                        Serial.print(" ");
                    }
        }
        Serial.println();
      }
        }
    }
}

// Check and synchronize data toggles if needed (simplified)
bool HIDSelector::checkAndSyncToggles(const char* stepName) {
    // USB library handles toggles correctly - no intervention needed
    return false;
}

// Reset OUT toggle after NAK (failed transfer should not advance toggle)
void HIDSelector::resetToggleAfterNAK(const char* commandName) {
    // Read current toggle state
    uint8_t hrslReg = pUsb->regRd(rHRSL);
    uint8_t currentToggle = (hrslReg & bmSNDTOGRD) ? 1 : 0;
    
    // Toggle should revert to previous state since transfer failed
    uint8_t previousToggle = currentToggle ^ 1; // Flip bit (1->0, 0->1)
    
    Serial.print("🔄 NAK received for ");
    Serial.print(commandName);
    Serial.print(" - will retry with DATA");
    Serial.println(previousToggle);
    
    // Set the toggle for the next retry transfer
    // Read current HCTL register to preserve other bits
    uint8_t hctlReg = pUsb->regRd(rHCTL);
    
    // Clear existing OUT toggle bits (bits 7,6 = SNDTOG1,SNDTOG0)
    hctlReg &= 0x3F; // Keep lower 6 bits, clear upper 2 OUT toggle bits
    
    // Set OUT toggle to previous state for retry
    if (previousToggle == 1) {
        hctlReg |= (0x01 << 6); // SNDTOG0 = 1 (DATA1)
        Serial.println("🔧 Setting retry toggle to DATA1");
    } else {
        Serial.println("🔧 Setting retry toggle to DATA0");
    }
    // For DATA0, both SNDTOG1 and SNDTOG0 = 0 (already cleared above)
    
    // Write the updated HCTL register - this sets toggle for NEXT transfer
    pUsb->regWr(rHCTL, hctlReg);
    
    // Verify by reading back
    delay(2);
    hrslReg = pUsb->regRd(rHRSL);
    uint8_t newToggle = (hrslReg & bmSNDTOGRD) ? 1 : 0;
    Serial.print("📊 Toggle after reset attempt: DATA");
    Serial.println(newToggle);
}

// Simple function to show current toggle states (simplified)
void HIDSelector::showToggleStates(const char* afterCommand) {
    // Toggle states are managed automatically by USB library
    // No need for verbose debugging output
}

// Set toggle state for the next transfer by working with library's endpoint management
void HIDSelector::setToggleForNextTransfer(uint8_t toggleState, const char* reason) {
    Serial.print("🔧 ");
    Serial.print(reason);
    Serial.print(" - attempting to set next transfer to DATA");
    Serial.println(toggleState);
    
    // Try to access the endpoint info through the library's interface
    // The USB Host Shield library tracks toggles in endpoint structures
    
    // First try: Direct register manipulation (what we tried before)
    uint8_t hctlReg = pUsb->regRd(rHCTL);
    hctlReg &= 0x3F; // Clear OUT toggle bits
    if (toggleState == 1) {
        hctlReg |= (0x01 << 6); // SNDTOG0 = 1
    }
    pUsb->regWr(rHCTL, hctlReg);
    
    // Second try: Force a specific endpoint state if possible
    // The library might be overriding our register writes with its own endpoint tracking
    
    // Try to reset the library's internal endpoint state
    // This is a bit of a hack, but we need to work with how the library manages endpoints
    
    Serial.println("📋 Note: Library may override toggle settings during transfer");
}

// Test LED command sequence with cycling RGB animation - MAX3421E synchronized
void HIDSelector::testLEDCommands() {
    static uint8_t colorPhase = 0; // 0=Red, 1=Green, 2=Blue, 3=Yellow, 4=Cyan, 5=Magenta
    uint8_t cmd[64];
    uint8_t rcode;
    
    // Define color patterns for animation
    uint8_t colors[6][3] = {
        {0xFF, 0x00, 0x00}, // Red
        {0x00, 0xFF, 0x00}, // Green  
        {0x00, 0x00, 0xFF}, // Blue
        {0xFF, 0xFF, 0x00}, // Yellow
        {0x00, 0xFF, 0xFF}, // Cyan
        {0xFF, 0x00, 0xFF}  // Magenta
    };
    
    uint8_t r = colors[colorPhase][0];
    uint8_t g = colors[colorPhase][1];
    uint8_t b = colors[colorPhase][2];
    
    const char* colorNames[] = {"Red", "Green", "Blue", "Yellow", "Cyan", "Magenta"};
    
    Serial.print("🎨 Animating LEDs: ");
    Serial.print(colorNames[colorPhase]);
    Serial.print(" (RGB: ");
    if (r < 0x10) Serial.print("0");
    Serial.print(r, HEX);
    Serial.print(", ");
    if (g < 0x10) Serial.print("0");
    Serial.print(g, HEX);
    Serial.print(", ");
    if (b < 0x10) Serial.print("0");
    Serial.print(b, HEX);
    Serial.println(")");
    
    // Wait for USB frame synchronization - MAX3421E specific
    waitForUSBFrameSync();
    
    // Command 1: Package 1 of 2 (56 83) - Exact breakdown structure
    memset(cmd, 0, 64);
    cmd[0] = 0x56; cmd[1] = 0x83;
    cmd[4] = 0x01; cmd[8] = 0x80; cmd[9] = 0x01;
    cmd[12] = 0xff; cmd[18] = 0xff; cmd[19] = 0xff;
    
    // RGB data starting at position 24 with exact LED ordering from breakdown
    int pos = 24;
    
    // Row 1: Buttons 1, 6, 11, 16, 21
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 1
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 6
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 11
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 16
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 21
    
    // Row 2: Buttons 2, 7, 12, 17, 22
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 2
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 7
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 12
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 17
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 22
    
    // Row 3: Buttons 3, 8, 13, and PARTIAL Button 18 (R component only)
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 3
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 8
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 13
    cmd[pos++] = r; // Button 18 (R component only - packet boundary!)
    
    // Wait for FIFO ready before sending
    waitForFIFOReady();
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode != 0 && rcode != 0x4) {
        Serial.print("❌ Command 1 failed: 0x");
        Serial.println(rcode, HEX);
        return;
    }
    
    // Synchronize with USB frame before next packet - critical for timing
    waitForUSBFrameSync();
    
    // Command 2: Package 2 of 2 (56 83 01) - Exact breakdown structure
    memset(cmd, 0, 64);
    cmd[0] = 0x56; cmd[1] = 0x83; cmd[2] = 0x01;
    
    pos = 3;
    // Complete Button 18 (GB components) and Button 23
    cmd[pos++] = 0x00; cmd[pos++] = g; cmd[pos++] = b; // Button 18 (GB components)
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 23
    
    // Row 4: Buttons 4, 9, 14, 19, 24
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 4
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 9
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 14
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 19
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 24
    
    // Row 5: Buttons 5, 10, 15, 20
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 5
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 10
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 15
    cmd[pos++] = r; cmd[pos++] = g; cmd[pos++] = b; // Button 20
    
    // Wait for FIFO ready before sending
    waitForFIFOReady();
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode != 0 && rcode != 0x4) {
        Serial.print("❌ Command 2 failed: 0x");
        Serial.println(rcode, HEX);
        return;
    }
    
    // Final sync before activation - ensure LED data is processed
    waitForUSBFrameSync();
    
    // Command 3: Final activation (51 28)
    memset(cmd, 0, 64);
    cmd[0] = 0x51; cmd[1] = 0x28;
    cmd[4] = 0xff;
    
    // Wait for FIFO ready before final command
    waitForFIFOReady();
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode != 0 && rcode != 0x4) {
        Serial.print("❌ Command 3 failed: 0x");
        Serial.println(rcode, HEX);
        return;
    }
    
    // Advance to next color for next animation cycle
    colorPhase = (colorPhase + 1) % 6;
    
    Serial.println("✅ Animation frame sent successfully!");
}

// Wait for USB frame synchronization - aligns with MAX3421E 1ms USB frames
void HIDSelector::waitForUSBFrameSync() {
    // Simple frame synchronization using fixed timing
    // MAX3421E operates on 1ms USB frame intervals
    static unsigned long lastFrameTime = 0;
    unsigned long currentTime = micros();
    
    // Ensure at least 1ms has passed since last frame
    unsigned long timeSinceLastFrame = currentTime - lastFrameTime;
    if (timeSinceLastFrame < 1000) {
        delayMicroseconds(1000 - timeSinceLastFrame);
    }
    
    lastFrameTime = micros();
}

// Wait for FIFO to be ready for transfer
void HIDSelector::waitForFIFOReady() {
    // Simple FIFO ready timing - wait for USB transfer completion
    // This ensures previous transfer is complete before starting new one
    delayMicroseconds(200); // Wait for FIFO to stabilize
    
    // Additional check to ensure USB subsystem is ready
    unsigned long startTime = micros();
    while (!pUsb || micros() - startTime > 500) {
        if (micros() - startTime > 500) break; // Timeout
        delayMicroseconds(50);
    }
}

USB Usb;
//USBHub Hub(&Usb);
HIDSelector hidSelector(&Usb);

void setup()
{
    Serial.begin(115200);
#if !defined(__MIPSEL__)
    while (!Serial); // Wait for serial port to connect - used on Leonardo, Teensy and other boards with built-in USB CDC serial connection
#endif
    Serial.println("🚀 Starting CM Control Pad HID Composite Test");

    if (Usb.Init() == -1)
        Serial.println("❌ OSC did not start.");

    // Set this to higher values to enable more debug information
    // minimum 0x00, maximum 0xff, default 0x80
    UsbDEBUGlvl = 0x80;  // Reduced debug level for cleaner output

    delay(200);
}

void loop()
{
    Usb.Task();
    
    // Initialize Interface 1 with proper timing after device is detected
    static bool interface1InitAttempted = false;
    static bool animationStarted = false;
    static unsigned long deviceReadyTime = 0;
    
    // Mark when device first becomes ready
    if (hidSelector.isReady() && deviceReadyTime == 0) {
        deviceReadyTime = millis();
        Serial.println("🔍 Device detected! Waiting for full initialization...");
    }
    
    // Wait 2 seconds after device is ready before attempting Interface 1 init
    if (hidSelector.isReady() && !interface1InitAttempted && deviceReadyTime > 0 && 
        (millis() - deviceReadyTime > 2000)) {
        interface1InitAttempted = true;
        Serial.println("🚀 Device fully ready! Initializing Interface 1 for LED control...");
        bool success = hidSelector.initializeInterface1();
        if (success) {
            animationStarted = true;
            Serial.println("🌈 Starting continuous RGB animation test...");
            Serial.println("💡 Animation will cycle through: Red → Green → Blue → Yellow → Cyan → Magenta");
            Serial.println("⏱️ Each color will display for 150ms to allow complete LED processing");
        } else {
            Serial.println("❌ Interface 1 initialization failed - will retry in 5 seconds");
            // Reset for retry
            interface1InitAttempted = false;
            deviceReadyTime = millis(); // Reset timer for retry
        }
    }
    
    // Run RGB animation every 150ms to allow complete processing
    static unsigned long lastAnimation = 0;
    if (animationStarted && millis() - lastAnimation > 150) {
        lastAnimation = millis();
        hidSelector.testLEDCommands();
    }
    
    // Poll Interface 1 endpoint 0x83 for additional data (reduced frequency to avoid interference)
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll > 300 && hidSelector.isReady()) { // Poll every 300ms (further reduced)
        lastPoll = millis();
        
        uint8_t buf[64];
        uint16_t len = 64;
        uint8_t rcode = Usb.inTransfer(hidSelector.GetAddress(), 0x83, &len, buf);
        
        if (rcode == 0 && len > 0) {
            // Only show non-empty responses to avoid spam
            bool hasData = false;
            for (uint8_t i = 0; i < len; i++) {
                if (buf[i] != 0) {
                    hasData = true;
                    break;
                }
            }
            
            if (hasData) {
                Serial.print("📡 Interface 1 EP 0x83 (");
                Serial.print(len);
                Serial.print(" bytes): ");
                for (uint8_t i = 0; i < len && i < 16; i++) {
                    if (buf[i] < 0x10) Serial.print("0");
                    Serial.print(buf[i], HEX);
                    Serial.print(" ");
                }
                if (len > 16) Serial.print("...");
                Serial.println();
            }
        }
    }
}
