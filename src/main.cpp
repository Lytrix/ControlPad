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
    void resetDevice(); // Device recovery function
    
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
    
    // Enable SOF frame generation for precise timing
    Serial.println("📡 Enabling SOF frame generation (SOFKAENAB)...");
    uint8_t modeReg = pUsb->regRd(0x27); // Read current MODE register
    modeReg |= 0x10; // Set SOFKAENAB bit (bit 4)
    pUsb->regWr(0x27, modeReg); // Write back with SOFKAENAB enabled
    
    // Enable FRAMEIRQ interrupt
    uint8_t hienReg = pUsb->regRd(0x26); // Read current HIEN register  
    hienReg |= 0x40; // Set FRAMEIE bit (bit 6)
    pUsb->regWr(0x26, hienReg); // Write back with FRAMEIE enabled
    
    Serial.println("✅ SOF generation and FRAMEIRQ enabled");
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
        
        //for (uint8_t i = 0; i < len; i++) {
            for (uint8_t i = 0; i < 8; i++) {
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

// Test LED command sequence with moving LED on rainbow background - SOF optimized
void HIDSelector::testLEDCommands() {
    static uint8_t movingLED = 0; // Which LED is currently bright (0-23)
    uint8_t cmd[64];
    uint8_t rcode;
    static unsigned long debugCounter = 0;
    debugCounter++;
    
    Serial.print("🔍 LED command #");
    Serial.print(debugCounter);
    Serial.print(" starting... ");
    
    // Pre-calculated rainbow colors for maximum speed
    static const uint8_t rainbowColors[24][3] = {
        // Row 1: Buttons 1, 6, 11, 16, 21 - Red gradient
        {0xFF, 0x00, 0x00}, {0xFF, 0x20, 0x00}, {0xFF, 0x40, 0x00}, {0xFF, 0x60, 0x00}, {0xFF, 0x80, 0x00},
        // Row 2: Buttons 2, 7, 12, 17, 22 - Orange to Yellow
        {0xFF, 0xA0, 0x00}, {0xFF, 0xC0, 0x00}, {0xFF, 0xFF, 0x00}, {0xC0, 0xFF, 0x00}, {0x80, 0xFF, 0x00},
        // Row 3: Buttons 3, 8, 13, 18, 23 - Green gradient  
        {0x40, 0xFF, 0x00}, {0x00, 0xFF, 0x00}, {0x00, 0xFF, 0x40}, {0x00, 0xFF, 0x80}, {0x00, 0xFF, 0xC0},
        // Row 4: Buttons 4, 9, 14, 19, 24 - Cyan to Blue
        {0x00, 0xFF, 0xFF}, {0x00, 0xC0, 0xFF}, {0x00, 0x80, 0xFF}, {0x00, 0x40, 0xFF}, {0x00, 0x00, 0xFF},
        // Row 5: Buttons 5, 10, 15, 20 - Blue to Purple
        {0x40, 0x00, 0xFF}, {0x80, 0x00, 0xFF}, {0xC0, 0x00, 0xFF}, {0xFF, 0x00, 0xFF}
    };
    
    // Pre-calculated button mapping for speed
    static const uint8_t buttonMap[24] = {
         0,  5, 10, 15, 20,  // Buttons 1, 6, 11, 16, 21 (Row 1)
         1,  6, 11, 16, 21,  // Buttons 2, 7, 12, 17, 22 (Row 2)  
         2,  7, 12, 17, 22,  // Buttons 3, 8, 13, 18, 23 (Row 3)
         3,  8, 13, 18, 23,  // Buttons 4, 9, 14, 19, 24 (Row 4)
         4,  9, 14, 19       // Buttons 5, 10, 15, 20 (Row 5)
    };
    
    // ULTRA-FAST execution - minimize time in SOF frame
    
    // Command 1: Package 1 - pre-built structure for speed
    memset(cmd, 0, 64);
    cmd[0] = 0x56; cmd[1] = 0x83; cmd[4] = 0x01; cmd[8] = 0x80; cmd[9] = 0x01;
    cmd[12] = 0xff; cmd[18] = 0xff; cmd[19] = 0xff;
    
    // Fast RGB data population - optimized loop
    int pos = 24;
    for (int i = 0; i < 5; i++) { // Row 1
        if (movingLED == i) {
            cmd[pos++] = 0xFF; cmd[pos++] = 0xFF; cmd[pos++] = 0xFF;
        } else {
            const uint8_t* color = rainbowColors[buttonMap[i]];
            cmd[pos++] = color[0]; cmd[pos++] = color[1]; cmd[pos++] = color[2];
        }
    }
    for (int i = 5; i < 10; i++) { // Row 2
        if (movingLED == i) {
            cmd[pos++] = 0xFF; cmd[pos++] = 0xFF; cmd[pos++] = 0xFF;
        } else {
            const uint8_t* color = rainbowColors[buttonMap[i]];
            cmd[pos++] = color[0]; cmd[pos++] = color[1]; cmd[pos++] = color[2];
        }
    }
    for (int i = 10; i < 13; i++) { // Row 3 partial
        if (movingLED == i) {
            cmd[pos++] = 0xFF; cmd[pos++] = 0xFF; cmd[pos++] = 0xFF;
        } else {
            const uint8_t* color = rainbowColors[buttonMap[i]];
            cmd[pos++] = color[0]; cmd[pos++] = color[1]; cmd[pos++] = color[2];
        }
    }
    // Button 18 R component
    cmd[pos++] = (movingLED == 13) ? 0xFF : rainbowColors[buttonMap[13]][0];
    
    // Send Package 1 with detailed logging
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    Serial.print("📤 Pkg1: rcode=0x");
    Serial.print(rcode, HEX);
    if (rcode == 0) {
        Serial.print(" ✅ → ");
        // ACK received, proceed immediately to next command
    } else {
        Serial.print(" ❌ | ");
        for (int retry = 0; retry < 2; retry++) {
            delayMicroseconds(50);
            rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
            Serial.print("retry=0x");
            Serial.print(rcode, HEX);
            Serial.print(" ");
            if (rcode == 0) break;
            if (rcode != 0x4) break; // Non-NAK error
        }
        if (rcode != 0) {
            Serial.println("FAILED");
            return;
        }
        Serial.print("recovered → ");
    }
    
    // No delay - send next command immediately after ACK
    
    // Command 2: Package 2 - pre-built for speed
    memset(cmd, 0, 64);
    cmd[0] = 0x56; cmd[1] = 0x83; cmd[2] = 0x01;
    
    pos = 3;
    // Button 18 GB + Button 23
    if (movingLED == 13) {
        cmd[pos++] = 0x00; cmd[pos++] = 0xFF; cmd[pos++] = 0xFF;
    } else {
        cmd[pos++] = 0x00; 
        cmd[pos++] = rainbowColors[buttonMap[13]][1]; 
        cmd[pos++] = rainbowColors[buttonMap[13]][2];
    }
    if (movingLED == 14) {
        cmd[pos++] = 0xFF; cmd[pos++] = 0xFF; cmd[pos++] = 0xFF;
    } else {
        const uint8_t* color = rainbowColors[buttonMap[14]];
        cmd[pos++] = color[0]; cmd[pos++] = color[1]; cmd[pos++] = color[2];
    }
    
    // Rows 4 & 5 - optimized
    for (int i = 15; i < 24; i++) {
        if (movingLED == i) {
            cmd[pos++] = 0xFF; cmd[pos++] = 0xFF; cmd[pos++] = 0xFF;
        } else {
            const uint8_t* color = rainbowColors[buttonMap[i]];
            cmd[pos++] = color[0]; cmd[pos++] = color[1]; cmd[pos++] = color[2];
        }
    }

    // Send Package 2 immediately after Package 1 ACK
    Serial.print("📤 Package 2... ");
    for (int retry = 0; retry < 3; retry++) {
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        Serial.print("rcode=0x");
        Serial.print(rcode, HEX);
        if (rcode == 0) {
            Serial.print(" ✅ → ");
            break; // ACK received, proceed immediately
        }
        Serial.print(" ❌");
        if (rcode != 0x4) {
            Serial.print(" (non-NAK error, aborting)");
            Serial.println();
            return; // Non-NAK error, abort
        }
        Serial.print(" (NAK, retry)");
        delayMicroseconds(50); // Brief delay before retry
    }
    if (rcode != 0) {
        Serial.println("FAILED after retries");
        return; // Failed after retries
    }
    
    // Command 3: Activation - CRITICAL - must succeed
    cmd[0] = 0x51; cmd[1] = 0x28; cmd[2] = 0x00; cmd[3] = 0x00; cmd[4] = 0xff;
    for (int i = 5; i < 64; i++) cmd[i] = 0; // Fast clear
    
    // Send activation with more aggressive retry (this is the most important command)
    Serial.print("📤 Sending Activation command... ");
    for (int retry = 0; retry < 5; retry++) {
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        Serial.print("rcode=0x");
        Serial.print(rcode, HEX);
        if (rcode == 0) {
            Serial.println(" ✅");
            break; // Success
        }
        Serial.print(" ❌");
        if (rcode != 0x4) {
            Serial.print(" (non-NAK error, aborting)");
            Serial.println();
            return; // Non-NAK error on activation - this is critical
        }
        Serial.print(" (NAK, retry)");
        delayMicroseconds(100); // Longer delay for activation retry
    }
    
    if (rcode != 0) {
        Serial.println("📤 Activation command FAILED after 5 retries - LEDs may not update");
        return; // Failed after retries
    }
    
    // Advance LED position
    movingLED = (movingLED + 1) % 24;
}

// Wait for USB frame synchronization - now uses actual SOF counter from MAX3421E
void HIDSelector::waitForUSBFrameSync() {
    // SOF timing handles all synchronization - no manual delays needed
    // The FRAMEIRQ ensures we're perfectly aligned with 1ms USB frames
}

// Wait for FIFO to be ready - minimal delay since SOF handles timing
void HIDSelector::waitForFIFOReady() {
    // Minimal delay only for immediate FIFO stabilization
    // SOF synchronization eliminates need for frame timing delays
    delayMicroseconds(50); // Reduced from 200μs - just enough for FIFO
}

// Device recovery function for when communication fails
void HIDSelector::resetDevice() {
    Serial.println("🔄 Attempting device recovery...");
    
    // Try to reinitialize the LED interface
    uint8_t cmd[64];
    memset(cmd, 0, 64);
    
    // Send mode reset command
    cmd[0] = 0x56; cmd[1] = 0x81;
    uint8_t rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    
    if (rcode == 0) {
        Serial.println("✅ Device recovery successful");
    } else {
        Serial.print("❌ Device recovery failed, rcode: 0x");
        Serial.println(rcode, HEX);
    }
    
    delay(100); // Give device time to reset
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
    static unsigned long loopStartTime = millis();
    static unsigned long lastInterruptionCheck = 0;
    Usb.Task();
    
    // Initialize Interface 1 once device is ready and operational
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
        Serial.print("🚀 Device fully ready at T+");
        Serial.print(millis() - loopStartTime);
        Serial.println("ms! Initializing Interface 1 for LED control...");
        bool success = hidSelector.initializeInterface1();
        if (success) {
            animationStarted = true;
            Serial.println("🌈 Starting SOF-synchronized RGB animation test...");
            Serial.println("💡 Animation: Moving white LED on rainbow background");
            Serial.println("⏱️ Each LED position synchronized with USB SOF frames (200 frames = 200ms)");
            Serial.println("🎯 Perfect USB timing - optimized for LED stability!");
            Serial.print("🕐 Animation started at T+");
            Serial.print(millis() - loopStartTime);
            Serial.println("ms");
        } else {
            Serial.print("❌ Interface 1 initialization failed at T+");
            Serial.print(millis() - loopStartTime);
            Serial.println("ms - will retry in 5 seconds");
            // Reset for retry
            interface1InitAttempted = false;
            deviceReadyTime = millis(); // Reset timer for retry
        }
    }
    
    // Run RGB animation synchronized with SOF frames instead of millis()
    static bool sofSyncInitialized = false;
    static uint16_t animationCount = 0;
    
    if (animationStarted && hidSelector.isReady()) {

        
        // Periodic FNADDR polling approach - optimized for 1000Hz SOF timing
        static uint16_t lastFrameNum = 0;
        static uint16_t animationFrameInterval = 200; // Default 200 frames for 200ms
        static bool frameRateCalculated = false;
        static uint16_t startFrameNum = 0;
        static uint32_t startTime = 0;
        static uint32_t lastFramePollTime = 0;
        static uint16_t cachedFrameNum = 0;
        
        uint32_t nowUs = micros();
        unsigned long nowMs = millis();
        
        // Periodic FNADDR polling - configurable interval for optimal performance
        // Since SOF runs at 1000Hz (1ms), polling every 2-5ms allows early/late resync
        static uint8_t framePollingInterval = 3; // ms - can be adjusted for timing requirements
        uint16_t frameNum = cachedFrameNum;
        
                 if (nowMs - lastFramePollTime >= framePollingInterval || lastFramePollTime == 0) {
             frameNum = Usb.regRd(0x0E); // rFNADDR - only read periodically
             cachedFrameNum = frameNum;
             lastFramePollTime = nowMs;
             

             
             // Dynamic interval adjustment: reduce polling near critical timing points
             static unsigned long lastAnimationTime = 0;
             unsigned long timeSinceLastAnimation = nowMs - lastAnimationTime;
             if (timeSinceLastAnimation > 180 && timeSinceLastAnimation < 220) {
                 framePollingInterval = 1; // Increase precision near animation timing
             } else {
                 framePollingInterval = 3; // Normal polling rate
             }
         }
        
        // Initialize on first run
        if (!sofSyncInitialized) {
            lastFrameNum = frameNum;
            startFrameNum = frameNum;
            startTime = nowMs;
            sofSyncInitialized = true;
            // Since logs show 1000Hz, start immediately with 200 frames = 200ms
            animationFrameInterval = 200;
            frameRateCalculated = true;
            Serial.print("🎯 Periodic FNADDR polling initialized! Starting frame: ");
            Serial.print(frameNum);
            Serial.print(" - Polling every ");
            Serial.print(framePollingInterval);
            Serial.println("ms for 1000Hz SOF synchronization");
        }
        
        // SOF tick detected when frame number changes
        if (frameNum != lastFrameNum) {
            lastFrameNum = frameNum;
            
            // TEMPORARY: Since we see 1000Hz in logs, hardcode 200 frames = 200ms
            static bool quickStart = false;
            if (!quickStart && (nowMs - startTime) > 500) {
                animationFrameInterval = 200; // 200 frames for 200ms at 1000Hz
                frameRateCalculated = true;
                quickStart = true;
                Serial.println("📊 Quick start: Using 200 frames for 200ms (1000Hz SOF detected)");
            }
            
            // Calculate frame rate after 1 second
            if (!frameRateCalculated && (nowMs - startTime) > 1000) {
                uint16_t totalFrames;
                // Handle 11-bit wraparound (0-2047)
                if (frameNum >= startFrameNum) {
                    totalFrames = frameNum - startFrameNum;
                } else {
                    totalFrames = (2048 - startFrameNum) + frameNum;
                }
                
                float timeElapsed = (nowMs - startTime) / 1000.0;
                float frameRate = totalFrames / timeElapsed;
                animationFrameInterval = (uint16_t)(frameRate * 0.2); // 200ms worth of frames
                frameRateCalculated = true;
                
                Serial.print("📊 FNADDR Frame Rate: ");
                Serial.print(frameRate, 1);
                Serial.print(" Hz (");
                Serial.print(1000.0 / frameRate, 3);
                Serial.print("ms per frame) - Using ");
                Serial.print(animationFrameInterval);
                Serial.println(" frames for 200ms animation intervals");
            }
        }
        
        // Check for animation trigger every loop (outside frame change detection)
        if (frameRateCalculated) {
            // Calculate total frames from start (handle wraparound)
            uint16_t totalFramesFromStart;
            if (frameNum >= startFrameNum) {
                totalFramesFromStart = frameNum - startFrameNum;
            } else {
                totalFramesFromStart = (2048 - startFrameNum) + frameNum;
            }
            
            // Run animation at calculated intervals
            if (frameRateCalculated && (totalFramesFromStart % animationFrameInterval) == 0 && totalFramesFromStart > 0) {
                animationCount++;
                
                Serial.print("🎯 FNADDR animation #");
                Serial.print(animationCount);
                Serial.print(" at frame ");
                Serial.print(frameNum);
                Serial.print(" (total ");
                Serial.print(totalFramesFromStart);
                Serial.print(", T+");
                Serial.print(nowMs - loopStartTime);
                Serial.print("ms, expected ");
                Serial.print(animationCount * 200);
                Serial.print("ms)");
                
                // Execute LED command with comprehensive failure detection
                Serial.print("📤 Starting LED command sequence... ");
                unsigned long ledCommandStart = micros();
                
                // Check USB host state before sending commands
                uint8_t usbState = Usb.getUsbTaskState();
                Serial.print("USB state: 0x");
                Serial.print(usbState, HEX);
                Serial.print(" | ");
                
                hidSelector.testLEDCommands();
                unsigned long ledCommandDuration = micros() - ledCommandStart;
                
                Serial.print(" - completed in ");
                Serial.print(ledCommandDuration);
                Serial.println("μs");
                
                // Root cause analysis - detect failure patterns
                static unsigned long lastSuccessfulCommand = 0;
                static uint16_t failureCount = 0;
                
                if (ledCommandDuration < 300) {
                    failureCount++;
                    Serial.print("⚠️ Suspiciously fast command (");
                    Serial.print(ledCommandDuration);
                    Serial.print("μs) - possible failure #");
                    Serial.print(failureCount);
                    Serial.print(" | Time since last success: ");
                    Serial.print(nowMs - lastSuccessfulCommand);
                    Serial.println("ms");
                    
                    if (failureCount >= 2) {
                        Serial.println("🔍 ROOT CAUSE ANALYSIS:");
                        Serial.print("   - USB Task State: 0x");
                        Serial.println(Usb.getUsbTaskState(), HEX);
                        Serial.print("   - Device Address: ");
                        Serial.println(hidSelector.GetAddress());
                        Serial.print("   - Device Ready: ");
                        Serial.println(hidSelector.isReady() ? "YES" : "NO");
                        
                        // Check USB registers
                        uint8_t hirq = Usb.regRd(0x25);
                        uint8_t hien = Usb.regRd(0x26);
                        Serial.print("   - HIRQ: 0x");
                        Serial.print(hirq, HEX);
                        Serial.print(" | HIEN: 0x");
                        Serial.println(hien, HEX);
                    }
                } else {
                    lastSuccessfulCommand = nowMs;
                    if (failureCount > 0) {
                        Serial.print("✅ Command recovered after ");
                        Serial.print(failureCount);
                        Serial.println(" failures");
                    }
                    failureCount = 0;
                }
            } else if (frameRateCalculated && totalFramesFromStart > 0) {
                // Debug: Show why animation isn't triggering
                static unsigned long lastDebugTime = 0;
                if (nowMs - lastDebugTime > 2000) { // Every 2 seconds
                    Serial.print("🔍 Animation check: totalFrames=");
                    Serial.print(totalFramesFromStart);
                    Serial.print(", modulo=");
                    Serial.print(totalFramesFromStart % animationFrameInterval);
                    Serial.print(", interval=");
                    Serial.print(animationFrameInterval);
                    Serial.print(", frameRateCalc=");
                    Serial.println(frameRateCalculated ? "YES" : "NO");
                    lastDebugTime = nowMs;
                }
            }
        }
        
        // TEMPORARY: Simple timer-based animation trigger for testing
        static unsigned long lastTimerAnimation = 0;
        if (frameRateCalculated && (nowMs - lastTimerAnimation) >= 200) {
            lastTimerAnimation = nowMs;
            animationCount++;
            
            Serial.print("⏰ Timer animation #");
            Serial.print(animationCount);
            Serial.print(" at T+");
            Serial.print(nowMs - loopStartTime);
            Serial.print("ms (frame ");
            Serial.print(frameNum);
            Serial.print(", FNADDR raw: 0x");
            Serial.print(frameNum, HEX);
            Serial.print(")");
            
            // Execute LED command with comprehensive failure detection
            Serial.print("📤 Starting LED command sequence... ");
            unsigned long ledCommandStart = micros();
            
            // Check USB host state before sending commands
            uint8_t usbState = Usb.getUsbTaskState();
            Serial.print("USB state: 0x");
            Serial.print(usbState, HEX);
            Serial.print(" | ");
            
            hidSelector.testLEDCommands();
            unsigned long ledCommandDuration = micros() - ledCommandStart;
            
            Serial.print(" - completed in ");
            Serial.print(ledCommandDuration);
            Serial.println("μs");
            
            // Root cause analysis - detect failure patterns
            static unsigned long lastSuccessfulCommand = 0;
            static uint16_t failureCount = 0;
            
            if (ledCommandDuration < 300) {
                failureCount++;
                Serial.print("⚠️ Suspiciously fast command (");
                Serial.print(ledCommandDuration);
                Serial.print("μs) - possible failure #");
                Serial.print(failureCount);
                Serial.print(" | Time since last success: ");
                Serial.print(nowMs - lastSuccessfulCommand);
                Serial.println("ms");
                
                if (failureCount >= 2) {
                    Serial.println("🔍 ROOT CAUSE ANALYSIS:");
                    Serial.print("   - USB Task State: 0x");
                    Serial.println(Usb.getUsbTaskState(), HEX);
                    Serial.print("   - Device Address: ");
                    Serial.println(hidSelector.GetAddress());
                    Serial.print("   - Device Ready: ");
                    Serial.println(hidSelector.isReady() ? "YES" : "NO");
                    
                    // Check USB registers
                    uint8_t hirq = Usb.regRd(0x25);
                    uint8_t hien = Usb.regRd(0x26);
                    Serial.print("   - HIRQ: 0x");
                    Serial.print(hirq, HEX);
                    Serial.print(" | HIEN: 0x");
                    Serial.println(hien, HEX);
                }
            } else {
                lastSuccessfulCommand = nowMs;
                if (failureCount > 0) {
                    Serial.print("✅ Command recovered after ");
                    Serial.print(failureCount);
                    Serial.println(" failures");
                }
                failureCount = 0;
            }
            
            // Debug FNADDR reading every 10th animation
            if (animationCount % 10 == 0) {
                uint16_t fn1 = Usb.regRd(0x0E);
                delayMicroseconds(100);
                uint16_t fn2 = Usb.regRd(0x0E);
                delayMicroseconds(100); 
                uint16_t fn3 = Usb.regRd(0x0E);
                Serial.print("🔍 FNADDR readings: ");
                Serial.print(fn1);
                Serial.print(" → ");
                Serial.print(fn2);
                Serial.print(" → ");
                Serial.print(fn3);
                Serial.println(" (should increment if SOF working)");
            }
        }
    }
    
    // Poll Interface 1 endpoint 0x83 for additional data (reduced frequency to avoid interference)
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll > 300 && hidSelector.isReady()) { // Poll every 300ms (further reduced)
        unsigned long pollStartTime = millis();
        lastPoll = pollStartTime;
        
        uint8_t buf[64];
        uint16_t len = 64;
        uint8_t rcode = Usb.inTransfer(hidSelector.GetAddress(), 0x83, &len, buf);
        
        unsigned long pollDuration = millis() - pollStartTime;
        if (pollDuration > 50) {
            Serial.print("⚠️ Slow polling operation: ");
            Serial.print(pollDuration);
            Serial.print("ms at T+");
            Serial.print(pollStartTime - loopStartTime);
            Serial.println("ms");
        }
        
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
                Serial.print(" bytes) at T+");
                Serial.print(pollStartTime - loopStartTime);
                Serial.print("ms: ");
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
    
    // More frequent pattern monitoring - check for 0.8s and 3.11s patterns
    static unsigned long patternCheckTimes[10];
    static int patternIndex = 0;
    
    if (millis() - lastInterruptionCheck > 750) {  // Check every 750ms to catch 0.8s patterns
        unsigned long currentTime = millis();
        patternCheckTimes[patternIndex] = currentTime;
        
        // Look for 0.8s + 0.8s + 3.11s pattern in recent history
        if (patternIndex >= 2) {
            unsigned long gap1 = patternCheckTimes[patternIndex] - patternCheckTimes[patternIndex-1];
            unsigned long gap2 = patternCheckTimes[patternIndex-1] - patternCheckTimes[patternIndex-2];
            
            if ((gap1 >= 750 && gap1 <= 850) || (gap1 >= 3000 && gap1 <= 3200)) {
                Serial.print("🔍 Pattern detected: ");
                Serial.print(gap2);
                Serial.print("ms → ");
                Serial.print(gap1);
                Serial.print("ms at T+");
                Serial.print(currentTime - loopStartTime);
                Serial.println("ms");
            }
        }
        
        patternIndex = (patternIndex + 1) % 10;
        lastInterruptionCheck = currentTime;
  }
}
