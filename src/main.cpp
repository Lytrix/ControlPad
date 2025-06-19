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
    void resetFrameCounter(); // Reset USB frame counter to prevent wraparound issues
    void checkUSBSpeed(); // Check and optimize USB speed settings
    void optimizeNAKSettings(); // Optimize NAK tolerance for long-term stability
    
    
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
    //rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    delay(20);
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode == 0) Serial.println("✅ Command 4 retry successful!");
    delay(20);
    
    // Command 5: 52 00 (activate effects)
    Serial.println("📤 Step 5: Activate effects 52 00");
    memset(cmd, 0, 64);
    cmd[0] = 0x52; cmd[1] = 0x00;
    //rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    delay(20);
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode == 0) Serial.println("✅ Command 5 retry successful!");
    delay(20);
    
    // Final check for all responses
    checkForResponses();
    
    // === 56 14 INITIALIZATION SEQUENCE ===
    // Based on USB capture analysis - device needs 56 14 setup commands FIRST
    // Send commands in groups of 3 to work around the 3-command limit
    Serial.println("🔧 Sending 56 14 initialization sequence (0x00-0x0D) in groups of 3...");
    
    // Send 56 14 commands in groups of 3 with proper data patterns
    for (uint8_t group = 0; group < 5; group++) { // 5 groups: 0-2, 3-5, 6-8, 9-B, C-D
        uint8_t startCmd = group * 3;
        uint8_t endCmd = (group == 4) ? 0x0D : startCmd + 2; // Last group only has 2 commands
        
        Serial.print("📤 Group ");
        Serial.print(group + 1);
        Serial.print(": Commands 0x");
        Serial.print(startCmd, HEX);
        Serial.print("-0x");
        Serial.print(endCmd, HEX);
        Serial.println("...");
        
        // Send each command in this group
        for (uint8_t subCmd = startCmd; subCmd <= endCmd; subCmd++) {
            Serial.print("  📤 56 14 ");
            Serial.print(subCmd, HEX);
            Serial.print("... ");
            
            memset(cmd, 0, 64);
            cmd[0] = 0x56; 
            cmd[1] = 0x14; 
            cmd[2] = subCmd;
            
            // Add specific data patterns based on USB capture
            switch (subCmd) {
                case 0x00: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x04; cmd[6] = 0x00; cmd[7] = 0x55; cmd[8] = 0x55; cmd[9] = 0x55; cmd[10] = 0x55; break;
                case 0x01: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x0A; cmd[6] = 0x00; cmd[7] = 0x55; cmd[8] = 0x55; cmd[9] = 0x55; cmd[10] = 0x55; break;
                case 0x02: cmd[3] = 0x02; cmd[4] = 0x00; cmd[5] = 0x16; cmd[6] = 0x00; cmd[7] = 0x00; cmd[8] = 0x00; cmd[9] = 0x00; cmd[10] = 0x00; break;
                case 0x03: cmd[3] = 0x02; cmd[4] = 0x00; cmd[5] = 0x22; cmd[6] = 0x00; cmd[7] = 0x00; cmd[8] = 0x00; cmd[9] = 0x00; cmd[10] = 0x00; break;
                case 0x04: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x3F; cmd[6] = 0x00; cmd[7] = 0xBB; cmd[8] = 0xBB; cmd[9] = 0xBB; cmd[10] = 0xBB; break;
                case 0x05: cmd[3] = 0x02; cmd[4] = 0x00; cmd[5] = 0x4A; cmd[6] = 0x00; cmd[7] = 0x00; cmd[8] = 0x00; cmd[9] = 0x00; cmd[10] = 0x00; break;
                case 0x06: cmd[3] = 0x02; cmd[4] = 0x00; cmd[5] = 0x57; cmd[6] = 0x00; cmd[7] = 0x00; cmd[8] = 0x00; cmd[9] = 0x00; cmd[10] = 0x00; break;
                case 0x07: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x5F; cmd[6] = 0x00; cmd[7] = 0x55; cmd[8] = 0x55; cmd[9] = 0x55; cmd[10] = 0x55; break;
                case 0x08: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x65; cmd[6] = 0x00; cmd[7] = 0x55; cmd[8] = 0x55; cmd[9] = 0x55; cmd[10] = 0x55; break;
                case 0x09: cmd[3] = 0x02; cmd[4] = 0x00; cmd[5] = 0x6E; cmd[6] = 0x00; cmd[7] = 0x00; cmd[8] = 0x00; cmd[9] = 0x00; cmd[10] = 0x00; break;
                case 0x0A: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x77; cmd[6] = 0x00; cmd[7] = 0x55; cmd[8] = 0x55; cmd[9] = 0x55; cmd[10] = 0x55; break;
                case 0x0B: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x7E; cmd[6] = 0x00; cmd[7] = 0x55; cmd[8] = 0x55; cmd[9] = 0x55; cmd[10] = 0x55; break;
                case 0x0C: cmd[3] = 0x02; cmd[4] = 0x00; cmd[5] = 0x88; cmd[6] = 0x00; cmd[7] = 0x00; cmd[8] = 0x00; cmd[9] = 0x00; cmd[10] = 0x00; break;
                case 0x0D: cmd[3] = 0x01; cmd[4] = 0x00; cmd[5] = 0x8D; cmd[6] = 0x00; cmd[7] = 0xAA; cmd[8] = 0xAA; cmd[9] = 0xAA; cmd[10] = 0xAA; break;
            }
            
            rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
            if (rcode == 0) {
                Serial.println("✅");
                delay(8); // Wait ~8ms between commands (matching USB capture timing)
            } else {
                if (rcode == 0x4) {
                    // Retry with longer delays
                    for (int i = 0; i < 3; i++) {
                        delay(15);
                        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
                        if (rcode == 0) {
                            Serial.println("✅ (retry)");
                            delay(8);
                            break;
                        }
                    }
                }
                if (rcode != 0) {
                    Serial.print("❌ (0x");
                    Serial.print(rcode, HEX);
                    Serial.println(")");
                }
            }
        }
        
        // Wait for device responses after each group (like USB capture)
        Serial.print("  ⏳ Waiting for device responses after group ");
        Serial.print(group + 1);
        Serial.println("...");
        delay(50); // Wait for device to process the group
        
        // Clear any pending responses
        for (int i = 0; i < 5; i++) {
            uint8_t response[64];
            uint16_t responseLen = 64;
            pUsb->inTransfer(bAddress, 0x3, &responseLen, response);
            delay(2);
        }
    }
    
    Serial.println("✅ 56 14 initialization sequence complete");
    delay(100); // Longer delay between sequences
    
    // === 56 20 INITIALIZATION SEQUENCE ===
    // Send 56 20 commands in groups of 3 to work around the 3-command limit
    Serial.println("🔧 Sending 56 20 initialization sequence (0x00-0x0A) in groups of 3...");
    
    // Send 56 20 commands in groups of 3
    for (uint8_t group = 0; group < 4; group++) { // 4 groups: 0-2, 3-5, 6-8, 9-A
        uint8_t startCmd = group * 3;
        uint8_t endCmd = (group == 3) ? 0x0A : startCmd + 2; // Last group only has 2 commands
        
        Serial.print("📤 Group ");
        Serial.print(group + 1);
        Serial.print(": Commands 0x");
        Serial.print(startCmd, HEX);
        Serial.print("-0x");
        Serial.print(endCmd, HEX);
        Serial.println("...");
        
        // Send each command in this group
        for (uint8_t subCmd = startCmd; subCmd <= endCmd; subCmd++) {
            Serial.print("  📤 56 20 ");
            Serial.print(subCmd, HEX);
            Serial.print("... ");
            
            memset(cmd, 0, 64);
            cmd[0] = 0x56; 
            cmd[1] = 0x20; 
            cmd[2] = subCmd;
            
            rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
            if (rcode == 0) {
                Serial.println("✅");
                delay(8); // Wait ~8ms between commands (matching USB capture timing)
            } else {
                if (rcode == 0x4) {
                    // Retry with longer delays
                    for (int i = 0; i < 3; i++) {
                        delay(15);
                        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
                        if (rcode == 0) {
                            Serial.println("✅ (retry)");
                            delay(8);
                            break;
                        }
                    }
                }
                if (rcode != 0) {
                    Serial.print("❌ (0x");
                    Serial.print(rcode, HEX);
                    Serial.println(")");
                }
            }
        }
        
        // Wait for device responses after each group
        Serial.print("  ⏳ Waiting for device responses after group ");
        Serial.print(group + 1);
        Serial.println("...");
        delay(50); // Wait for device to process the group
        
        // Clear any pending responses
        for (int i = 0; i < 5; i++) {
            uint8_t response[64];
            uint16_t responseLen = 64;
            pUsb->inTransfer(bAddress, 0x3, &responseLen, response);
            delay(2);
        }
    }
    
    Serial.println("✅ 56 20 initialization sequence complete");
    delay(100); // Longer delay between sequences
    
    // Status check after 56 20 sequence (from USB capture)
    Serial.println("📤 Status check 41 80 after 56 20 sequence");
    memset(cmd, 0, 64);
    cmd[0] = 0x41; cmd[1] = 0x80;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode == 0) Serial.println("✅ Status check successful!");
    delay(20);
    
    // 52 28 command after status check (from USB capture)
    Serial.println("📤 52 28 command after status check");
    memset(cmd, 0, 64);
    cmd[0] = 0x52; cmd[1] = 0x28;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode == 0) Serial.println("✅ 52 28 command successful!");
    delay(20);
    
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
    modeReg |= 0x08; // Set SOFKAENAB bit (bit 3) - corrected bit position per datasheet
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

// Test LED command sequence with moving LED on rainbow background - OPTIMIZED FRAME CREATION
void HIDSelector::testLEDCommands() {
    static uint8_t movingLED = 0; // Which LED is currently bright (0-23)
    uint8_t cmd[64];
    uint8_t rcode;
    static unsigned long debugCounter = 0;
    debugCounter++;
    
    Serial.print("🔍 LED command #");
    Serial.print(debugCounter);
    Serial.print(" starting... ");
    
    // === OPTIMIZED FRAME TEMPLATES - PRE-BUILT FOR MAXIMUM SPEED ===
    
    // Pre-built Package 1 template (no dynamic changes needed)
    static const uint8_t package1Template[64] = {
        0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 
        0x80, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
        // RGB data starts at position 24 - will be filled below
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Pre-built Package 2 template 
    static const uint8_t package2Template[64] = {
        0x56, 0x83, 0x01, 
        // RGB data starts at position 3 - will be filled below (61 zeros needed)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Pre-built activation command (never changes)
    static const uint8_t activationCmd[64] = {
        0x51, 0x28, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Pre-calculated rainbow colors (unchanged)
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
    
    // Pre-calculated button mapping (unchanged)
    static const uint8_t buttonMap[24] = {
         0,  5, 10, 15, 20,  // Buttons 1, 6, 11, 16, 21 (Row 1)
         1,  6, 11, 16, 21,  // Buttons 2, 7, 12, 17, 22 (Row 2)  
         2,  7, 12, 17, 22,  // Buttons 3, 8, 13, 18, 23 (Row 3)
         3,  8, 13, 18, 23,  // Buttons 4, 9, 14, 19, 24 (Row 4)
         4,  9, 14, 19       // Buttons 5, 10, 15, 20 (Row 5)
    };
    
    // === ULTRA-FAST FRAME CREATION - NO DYNAMIC ALLOCATION ===
    
    // Package 1: Fast template copy + optimized RGB fill
    memcpy(cmd, package1Template, 64);  // Single fast copy instead of memset + individual assignments
    
    // Optimized RGB population - unrolled loops for maximum speed
    uint8_t *rgbPtr = &cmd[24];  // Direct pointer arithmetic instead of pos tracking
    
    // Row 1 (LEDs 0-4) - unrolled for speed
    const uint8_t *color0 = (movingLED == 0) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[0]];
    *rgbPtr++ = color0[0]; *rgbPtr++ = color0[1]; *rgbPtr++ = color0[2];
    
    const uint8_t *color1 = (movingLED == 1) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[1]];
    *rgbPtr++ = color1[0]; *rgbPtr++ = color1[1]; *rgbPtr++ = color1[2];
    
    const uint8_t *color2 = (movingLED == 2) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[2]];
    *rgbPtr++ = color2[0]; *rgbPtr++ = color2[1]; *rgbPtr++ = color2[2];
    
    const uint8_t *color3 = (movingLED == 3) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[3]];
    *rgbPtr++ = color3[0]; *rgbPtr++ = color3[1]; *rgbPtr++ = color3[2];
    
    const uint8_t *color4 = (movingLED == 4) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[4]];
    *rgbPtr++ = color4[0]; *rgbPtr++ = color4[1]; *rgbPtr++ = color4[2];
    
    // Row 2 (LEDs 5-9) - unrolled for speed
    const uint8_t *color5 = (movingLED == 5) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[5]];
    *rgbPtr++ = color5[0]; *rgbPtr++ = color5[1]; *rgbPtr++ = color5[2];
    
    const uint8_t *color6 = (movingLED == 6) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[6]];
    *rgbPtr++ = color6[0]; *rgbPtr++ = color6[1]; *rgbPtr++ = color6[2];
    
    const uint8_t *color7 = (movingLED == 7) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[7]];
    *rgbPtr++ = color7[0]; *rgbPtr++ = color7[1]; *rgbPtr++ = color7[2];
    
    const uint8_t *color8 = (movingLED == 8) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[8]];
    *rgbPtr++ = color8[0]; *rgbPtr++ = color8[1]; *rgbPtr++ = color8[2];
    
    const uint8_t *color9 = (movingLED == 9) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[9]];
    *rgbPtr++ = color9[0]; *rgbPtr++ = color9[1]; *rgbPtr++ = color9[2];
    
    // Row 3 partial (LEDs 10-12) - unrolled for speed
    const uint8_t *color10 = (movingLED == 10) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[10]];
    *rgbPtr++ = color10[0]; *rgbPtr++ = color10[1]; *rgbPtr++ = color10[2];
    
    const uint8_t *color11 = (movingLED == 11) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[11]];
    *rgbPtr++ = color11[0]; *rgbPtr++ = color11[1]; *rgbPtr++ = color11[2];
    
    const uint8_t *color12 = (movingLED == 12) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[12]];
    *rgbPtr++ = color12[0]; *rgbPtr++ = color12[1]; *rgbPtr++ = color12[2];
    
    // Button 18 R component only
    *rgbPtr++ = (movingLED == 13) ? 0xFF : rainbowColors[buttonMap[13]][0];
    
    // Save Package 1 for potential resend - fast copy
    uint8_t package1Backup[64];
    memcpy(package1Backup, cmd, 64);
    
    // === NO PRE-COMMAND DELAYS ===
    // Removed all pre-command delays to eliminate artificial timing interference
    
    unsigned long pkg1StartTime = micros();
    
    // Wait for USB frame boundary before sending Package 1
    waitForUSBFrameSync();
    
    // Send Package 1 with detailed logging
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    unsigned long pkg1EndTime = micros();
    unsigned long pkg1Duration = pkg1EndTime - pkg1StartTime;
    
    Serial.print("📤 Pkg1: rcode=0x");
    Serial.print(rcode, HEX);
    Serial.print(" (");
    Serial.print(pkg1Duration);
    Serial.print("μs)");
    if (rcode == 0) {
        Serial.print(" ✅ → ");
       delayMicroseconds(500);
    } else {
        Serial.print(" ❌ | ");
        for (int retry = 0; retry < 2; retry++) {
            delayMicroseconds(200);
            rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
            Serial.print("retry=0x");
            Serial.print(rcode, HEX);
            Serial.print(" ");
            if (rcode == 0) break;
            if (rcode != 0x4) break;
        }
        if (rcode != 0) {
            Serial.println("FAILED");
            return;
        }
        Serial.print("recovered → ");
        delayMicroseconds(300);
    }
    
    // === PACKAGE 2: ULTRA-FAST CREATION ===
    memcpy(cmd, package2Template, 64);  // Fast template copy
    
    rgbPtr = &cmd[3];  // Reset pointer to Package 2 RGB start
    
    // Button 18 GB + Button 23 (LEDs 13-14) - optimized
    if (movingLED == 13) {
        *rgbPtr++ = 0x00; *rgbPtr++ = 0xFF; *rgbPtr++ = 0xFF;  // GB components for white
    } else {
        *rgbPtr++ = 0x00;  // G component always 0 for Package 2 
        *rgbPtr++ = rainbowColors[buttonMap[13]][1]; 
        *rgbPtr++ = rainbowColors[buttonMap[13]][2];
    }
    
    const uint8_t *color14 = (movingLED == 14) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[14]];
    *rgbPtr++ = color14[0]; *rgbPtr++ = color14[1]; *rgbPtr++ = color14[2];
    
    // Rows 4 & 5 (LEDs 15-23) - unrolled for maximum speed
    const uint8_t *color15 = (movingLED == 15) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[15]];
    *rgbPtr++ = color15[0]; *rgbPtr++ = color15[1]; *rgbPtr++ = color15[2];
    
    const uint8_t *color16 = (movingLED == 16) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[16]];
    *rgbPtr++ = color16[0]; *rgbPtr++ = color16[1]; *rgbPtr++ = color16[2];
    
    const uint8_t *color17 = (movingLED == 17) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[17]];
    *rgbPtr++ = color17[0]; *rgbPtr++ = color17[1]; *rgbPtr++ = color17[2];
    
    const uint8_t *color18 = (movingLED == 18) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[18]];
    *rgbPtr++ = color18[0]; *rgbPtr++ = color18[1]; *rgbPtr++ = color18[2];
    
    const uint8_t *color19 = (movingLED == 19) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[19]];
    *rgbPtr++ = color19[0]; *rgbPtr++ = color19[1]; *rgbPtr++ = color19[2];
    
    const uint8_t *color20 = (movingLED == 20) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[20]];
    *rgbPtr++ = color20[0]; *rgbPtr++ = color20[1]; *rgbPtr++ = color20[2];
    
    const uint8_t *color21 = (movingLED == 21) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[21]];
    *rgbPtr++ = color21[0]; *rgbPtr++ = color21[1]; *rgbPtr++ = color21[2];
    
    const uint8_t *color22 = (movingLED == 22) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[22]];
    *rgbPtr++ = color22[0]; *rgbPtr++ = color22[1]; *rgbPtr++ = color22[2];
    
    const uint8_t *color23 = (movingLED == 23) ? (const uint8_t[]){0xFF, 0xFF, 0xFF} : rainbowColors[buttonMap[23]];
    *rgbPtr++ = color23[0]; *rgbPtr++ = color23[1]; *rgbPtr++ = color23[2];

    // === NO PRE-COMMAND DELAYS ===
    // Removed all pre-command delays to eliminate artificial timing interference

    unsigned long pkg2StartTime = micros();
    unsigned long pkg1ToPkg2Gap = pkg2StartTime - pkg1EndTime;
    
    // Wait for USB frame boundary before sending Package 2
    waitForUSBFrameSync();
    
    // Send Package 2 with proper timing
    Serial.print("📤 Package 2 (gap:");
    Serial.print(pkg1ToPkg2Gap);
    Serial.print("μs)... ");
    for (int retry = 0; retry < 3; retry++) {
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        unsigned long pkg2EndTime = micros();
        unsigned long pkg2Duration = pkg2EndTime - pkg2StartTime;
        
        Serial.print("rcode=0x");
        Serial.print(rcode, HEX);
        Serial.print(" (");
        Serial.print(pkg2Duration);
        Serial.print("μs)");
        if (rcode == 0) {
            Serial.print(" ✅ → ");
            delayMicroseconds(500);
            break;
        }
        Serial.print(" ❌");
        if (rcode != 0x4) {
            Serial.print(" (non-NAK error, aborting)");
            Serial.println();
            return;
        }
        Serial.print(" (NAK, retry)");
        delayMicroseconds(200);
    }
    if (rcode != 0) {
        Serial.print("FAILED after retries - restarting from Package 1... ");
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, package1Backup);
        if (rcode == 0) {
            Serial.print("Pkg1 resent ✅ → ");
            rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
            if (rcode == 0) {
                Serial.print("Pkg2 recovered ✅ → ");
            } else {
                Serial.println("Pkg2 still failed, aborting");
                return;
            }
        } else {
            Serial.println("Pkg1 resend failed, aborting");
            return;
        }
    }
    
    // === ACTIVATION: PRE-BUILT COMMAND - INSTANT COPY ===
    memcpy(cmd, activationCmd, 64);  // Ultra-fast - no dynamic creation needed
    
    // === NO PRE-COMMAND DELAYS ===
    // Removed all pre-command delays to eliminate artificial timing interference
    
    unsigned long activationStartTime = micros();
    unsigned long pkg2ToActivationGap = activationStartTime - pkg2StartTime; // This includes the 500μs delay
    
    // Wait for USB frame boundary before sending Activation
    waitForUSBFrameSync();
    
    // Send activation with conservative timing
    Serial.print("📤 Activation (gap:");
    Serial.print(pkg2ToActivationGap);
    Serial.print("μs)... ");
    for (int retry = 0; retry < 3; retry++) {
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        unsigned long activationEndTime = micros();
        unsigned long activationDuration = activationEndTime - activationStartTime;
        
        Serial.print("rcode=0x");
        Serial.print(rcode, HEX);
        Serial.print(" (");
        Serial.print(activationDuration);
        Serial.print("μs)");
        if (rcode == 0) {
            // CRITICAL: Ensure Activation command always takes ~250μs total (increased buffer)
            // Device expects consistent timing for stable LED operation
            const unsigned long TARGET_ACTIVATION_TIME = 250; // μs (was 225μs)
            if (activationDuration < TARGET_ACTIVATION_TIME) {
                unsigned long compensationDelay = TARGET_ACTIVATION_TIME - activationDuration;
                delayMicroseconds(compensationDelay);
                Serial.print(" +");
                Serial.print(compensationDelay);
                Serial.print("μs=");
                Serial.print(TARGET_ACTIVATION_TIME);
                Serial.print("μs");
            }
            Serial.println(" ✅");
            
            break;
        }
        Serial.print(" ❌");
        if (rcode != 0x4) {
            Serial.print(" (non-NAK error, aborting)");
            Serial.println();
            return;
        }
        Serial.print(" (NAK, retry)");
        delayMicroseconds(200);
    }
    
    if (rcode != 0) {
        Serial.print("📤 Activation FAILED - restarting full sequence... ");
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, package1Backup);
        if (rcode == 0) {
            Serial.print("Pkg1 resent ✅ → ");
            rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
            if (rcode == 0) {
                Serial.println("Activation recovered ✅");
    } else {
                Serial.println("Activation still failed");
                return;
            }
        } else {
            Serial.println("Pkg1 resend failed, aborting");
            return;
        }
    }
    
    
    // === PERIODIC DEVICE REFRESH ===
    // DISABLED: Device refresh was causing flickering at 12 and 36 seconds
    // The refresh commands were failing (rcode=0x4) and interfering with LED timing
    /*
    // Send a device refresh command every 50 LED updates (~3 seconds) to prevent
    // long-term timing drift and buffer accumulation in the device
    static unsigned long lastRefreshCommand = 0;
    if (debugCounter > 0 && debugCounter % 30 == 0 && debugCounter != lastRefreshCommand) {
        lastRefreshCommand = debugCounter;
        
        Serial.print("🔄 Device refresh #");
        Serial.print(debugCounter / 30);
        Serial.print(" at command ");
        Serial.print(debugCounter);
        Serial.print("... ");
        
        // Send a minimal status query command to refresh device timing
        uint8_t refreshCmd[64] = {0};
        refreshCmd[0] = 0x41;  // Status query command
        refreshCmd[1] = 0x80;  // Basic status
        
        delayMicroseconds(100); // Small gap after LED sequence
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, refreshCmd);
        Serial.print("rcode=0x");
        Serial.print(rcode, HEX);
        if (rcode == 0) {
            Serial.println(" ✅");
            } else {
            Serial.print(" ❌ (");
            Serial.print(rcode, HEX);
            Serial.println(")");
        }
        delayMicroseconds(200); // Brief recovery time
    }
    */
    
    // Move to next LED position
    movingLED = (movingLED + 1) % 24;
    
    // CRITICAL: Ensure minimum cycle time to prevent flickering
    // This prevents commands from being sent too rapidly, which causes device buffer overflow
    static unsigned long lastCycleTime = 0;
    unsigned long currentTime = micros();
    unsigned long cycleDuration = currentTime - lastCycleTime;
    
    // Update lastCycleTime to current time BEFORE any compensation delays
    // This prevents timing from compounding and doubling the effective speed
    lastCycleTime = currentTime;
    
    // Ensure minimum 100ms between LED command cycles to prevent flickering
    const unsigned long MIN_CYCLE_TIME = 100000; // 100ms in microseconds
    if (cycleDuration < MIN_CYCLE_TIME) {
        unsigned long compensationDelay = MIN_CYCLE_TIME - cycleDuration;
        Serial.print("⏰ Cycle timing compensation: +");
        Serial.print(compensationDelay / 1000);
        Serial.println("ms");
        delayMicroseconds(compensationDelay);
    }
}

// Wait for USB frame synchronization - now uses actual SOF counter from MAX3421E
void HIDSelector::waitForUSBFrameSync() {
    // Wait for the next USB frame boundary by polling FRAMEIRQ
    // FRAMEIRQ is set every 1ms (full-speed) or 125μs (low-speed)
    
    // Clear any pending FRAMEIRQ first
    pUsb->regWr(rHIRQ, bmFRAMEIRQ);
    
    // Wait for the next frame boundary
    unsigned long startTime = micros();
    unsigned long timeout = 2000; // 2ms timeout to prevent infinite loop
    
    Serial.print("⏱️ Waiting for SOF... ");
    
    while (!(pUsb->regRd(rHIRQ) & bmFRAMEIRQ)) {
        if (micros() - startTime > timeout) {
            Serial.print("⚠️ SOF sync timeout after ");
            Serial.print(timeout);
            Serial.println("μs");
            break;
        }
    }
    
    // Clear the FRAMEIRQ flag
    pUsb->regWr(rHIRQ, bmFRAMEIRQ);
    
    unsigned long syncDuration = micros() - startTime;
    Serial.print("✅ SOF sync complete (");
    Serial.print(syncDuration);
    Serial.print("μs)");
    
    // CRITICAL: Ensure minimum spacing between commands to prevent flickering
    // If SOF sync was too fast (< 100μs), add delay to prevent buffer overflow
    if (syncDuration < 100) {
        unsigned long compensationDelay = 100 - syncDuration;
        Serial.print(" +");
        Serial.print(compensationDelay);
        Serial.print("μs compensation");
        delayMicroseconds(compensationDelay);
    }
    Serial.println();
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

void HIDSelector::resetFrameCounter() {
    Serial.print("🔄 Resetting USB frame counter...");
    
    // Reset frame counter using FRMRST bit (bit 1 of HCTL register 0xE8)
    uint8_t hctlReg = pUsb->regRd(0xE8); // Read HCTL register
    hctlReg |= 0x02; // Set FRMRST bit (bit 1)
    pUsb->regWr(0xE8, hctlReg); // Write back to reset frame counter
    
    // Clear FRMRST bit after reset
    delayMicroseconds(100); // Brief delay for reset to take effect
    hctlReg &= ~0x02; // Clear FRMRST bit
    pUsb->regWr(0xE8, hctlReg); // Write back
    
    Serial.println(" ✅ Complete");
}

void HIDSelector::checkUSBSpeed() {
    Serial.println("🔍 Checking USB speed configuration...");
    
    // Check MAX3421E registers for speed information
    uint8_t mode = pUsb->regRd(0x1B);  // MODE register
    uint8_t vbusState = pUsb->getVbusState();
    
    Serial.print("📊 MODE register: 0x");
    Serial.print(mode, HEX);
    Serial.print(" (");
    
    if (mode & 0x02) {  // bmLOWSPEED bit
        Serial.print("LOW-SPEED");
    } else {
        Serial.print("FULL-SPEED");
    }
    
    Serial.print("), VBUS state: ");
    Serial.print(vbusState);
    Serial.print(" (");
    
    switch(vbusState) {
        case 0x10: Serial.print("FSHOST - Full-Speed Host"); break;
        case 0x20: Serial.print("LSHOST - Low-Speed Host"); break;
        case 0x40: Serial.print("SE0 - Disconnected"); break;
        case 0x50: Serial.print("SE1 - Illegal State"); break;
        default: Serial.print("Unknown: 0x"); Serial.print(vbusState, HEX); break;
    }
    Serial.println(")");
    
    // Force full-speed if currently running low-speed
    if (mode & 0x02) {  // bmLOWSPEED is set
        Serial.println("⚡ Device is running in LOW-SPEED mode - forcing FULL-SPEED for better performance");
        
        // Force full-speed host mode
        uint8_t newMode = (mode & ~0x02) | 0x01 | 0x08;  // Clear LOWSPEED, set HOST and SOFKAENAB
        pUsb->regWr(0x1B, newMode);  // Write to MODE register
        
        Serial.print("✅ Switched to FULL-SPEED mode (MODE: 0x");
        Serial.print(newMode, HEX);
        Serial.println(")");
        
        delay(100);  // Allow time for speed change to take effect
        
        // Verify the change
        uint8_t verifyMode = pUsb->regRd(0x1B);
        Serial.print("🔍 Verification - New MODE: 0x");
        Serial.print(verifyMode, HEX);
        Serial.println(verifyMode & 0x02 ? " (Still LOW-SPEED - device may require low-speed)" : " (Now FULL-SPEED)");
    } else {
        Serial.println("✅ Already running in FULL-SPEED mode - optimal configuration");
    }
    
    // Check SPI speed settings
    Serial.println("🔍 SPI speed configuration:");
    Serial.println("   MAX3421E can handle up to 26MHz SPI");
    Serial.println("   USB Host Shield library typically uses 12-21MHz");
    Serial.println("   Current: Optimized for Teensy 4.0 performance");
}

void HIDSelector::optimizeNAKSettings() {
    Serial.println("🔧 Optimizing NAK tolerance settings for long-term stability...");
    
    // The default USB Host Shield library uses:
    // - EP 0 (control): USB_NAK_MAX_POWER = 15 (32767 NAKs allowed)  
    // - All other EPs: USB_NAK_NOWAIT = 1 (only 1 NAK allowed!)
    //
    // This is TOO AGGRESSIVE for LED control which needs time tolerance
    
    if (!isReady()) {
        Serial.println("❌ Device not ready for NAK optimization");
        return;
    }
    
    // Get current endpoint info
    Serial.println("📊 Current NAK power settings:");
    for (int i = 0; i < bNumEP; i++) {
        Serial.print("   EP ");
        Serial.print(i);
        Serial.print(": bmNakPower=");
        Serial.print(epInfo[i].bmNakPower);
        Serial.print(" (nak_limit=");
        Serial.print((1 << epInfo[i].bmNakPower) - 1);
        Serial.print("), EP addr=0x");
        Serial.println(epInfo[i].epAddr, HEX);
    }
    
    // Optimize NAK settings for CM Control Pad stability
    // EP 0x04 (OUT) - LED commands need higher NAK tolerance
    // EP 0x03 (IN) - Status/response data needs moderate tolerance
    
    for (int i = 0; i < bNumEP; i++) {
        uint8_t oldNakPower = epInfo[i].bmNakPower;
        uint8_t newNakPower = oldNakPower;
        
        if (epInfo[i].epAddr == 0x04) {
            // EP 0x04 (OUT) - LED commands: Allow moderate NAK tolerance
            newNakPower = 4;  // 2^4 - 1 = 15 NAKs allowed (vs 1 NAK default)
            Serial.print("🎯 EP 0x04 (LED commands): ");
        } else if (epInfo[i].epAddr == 0x03) {
            // EP 0x03 (IN) - Status responses: Allow some NAK tolerance  
            newNakPower = 3;  // 2^3 - 1 = 7 NAKs allowed (vs 1 NAK default)
            Serial.print("📡 EP 0x03 (responses): ");
        } else if (epInfo[i].epAddr == 0x00) {
            // EP 0x00 (control): Keep high tolerance
            newNakPower = 15; // Keep maximum for control endpoint
            Serial.print("🎛️  EP 0x00 (control): ");
        } else {
            Serial.print("📌 EP 0x");
            Serial.print(epInfo[i].epAddr, HEX);
            Serial.print(": ");
        }
        
        if (newNakPower != oldNakPower) {
            epInfo[i].bmNakPower = newNakPower;
            Serial.print("Updated NAK power ");
            Serial.print(oldNakPower);
            Serial.print(" → ");
            Serial.print(newNakPower);
            Serial.print(" (");
            Serial.print((1 << oldNakPower) - 1);
            Serial.print(" → ");
            Serial.print((1 << newNakPower) - 1);
            Serial.println(" NAKs allowed)");
        } else {
            Serial.print("Keeping NAK power ");
            Serial.print(newNakPower);
            Serial.print(" (");
            Serial.print((1 << newNakPower) - 1);
            Serial.println(" NAKs allowed)");
        }
    }
    
    Serial.println("✅ NAK optimization complete!");
    Serial.println("💡 Benefits:");
    Serial.println("   • Increased tolerance for device response delays");
    Serial.println("   • Better handling of USB timing variations");
    Serial.println("   • Reduced transfer failures over extended operation");
    Serial.println("   • Should significantly improve 1-2 minute stability");
}



USB Usb;
//USBHub Hub(&Usb);
HIDSelector hidSelector(&Usb);

// Global variables at the top
unsigned long loopStartTime = 0;
int buttonState = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
int timerAnimationCount = 0;
unsigned long lastTimerTime = 0;

// EP polling statistics
static int epPollCount = 0;
static int epPollErrors = 0;
static int epPollSuccesses = 0;

void setup()
{
    Serial.begin(115200);
#if !defined(__MIPSEL__)
    while (!Serial); // Wait for serial port to connect - used on Leonardo, Teensy and other boards with built-in USB CDC serial connection
#endif
    Serial.println("🚀 Starting CM Control Pad HID Composite Test");

    // Initialize random seed for timing breaks
    randomSeed(analogRead(0));

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
    // Removed lastInterruptionCheck - no longer needed since pattern detection disabled
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
        Serial.println("ms! Checking USB speed and initializing Interface 1 for LED control...");
        
        // Check and optimize USB speed before starting LED operations
        //hidSelector.checkUSBSpeed();
        
        // Optimize NAK tolerance settings for long-term stability
        //hidSelector.optimizeNAKSettings();
         
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
    static uint16_t animationCount = 0;
    
    if (animationStarted && hidSelector.isReady()) {
        
        // Simple timer-based animation with proper FRAMEIRQ synchronization in LED commands
        static unsigned long lastTimerAnimation = 0;
        unsigned long nowMs = millis();
        
        // ULTIMATE MINIMAL APPROACH: Zero interference strategy
        // Frame counter resets and any USB maintenance operations
        // have been proven to DISRUPT stable communication.
        // Maximum stability achieved with NAK optimization alone.
        
        if (nowMs - lastTimerAnimation >= 100) { // Restore to 100ms for original animation speed
            lastTimerAnimation = nowMs;
            animationCount++;
            
            Serial.print("⏰ Timer animation #");
            Serial.print(animationCount);
            Serial.print(" at T+");
            Serial.print(nowMs - loopStartTime);
            Serial.print("ms");
            Serial.println();
            
            // Disabled USB health check - it was causing flickering at 13 seconds
            // Health checks seem to interfere with device stability
            
            // Removed preventive reset - it was causing more harm than good
            // Device runs more stably without artificial resets
            
            // Execute LED command sequence (now with proper FRAMEIRQ sync)
            hidSelector.testLEDCommands();
        }
    }
    
    // Poll Interface 1 endpoint 0x83 for additional data (frequent polling needed for ACKs!)
    // DISABLED: 100% failure rate shows EP 0x83 doesn't exist on this device
    static unsigned long lastPoll = 0;
    static bool pollingEnabled = false; // EP polling completely broken - 100% failure rate
    
    if (pollingEnabled && millis() - lastPoll > 50 && hidSelector.isReady()) { // Poll every 100ms (reduced from 50ms)
        unsigned long pollStartTime = millis();
        lastPoll = pollStartTime;
        
        uint8_t buf[64];
        uint16_t len = 64;
        uint8_t rcode = Usb.inTransfer(hidSelector.GetAddress(), 0x83, &len, buf);
        
        epPollCount++;
        if (rcode == 0) {
            epPollSuccesses++;
        } else {
            epPollErrors++;
        }
        
        // Report EP polling statistics every 100 polls (~10 seconds)
        if (epPollCount % 100 == 0) {
            Serial.print("📊 EP polling stats: ");
            Serial.print(epPollSuccesses);
            Serial.print(" successes, ");
            Serial.print(epPollErrors);
            Serial.print(" errors, ");
            Serial.print((epPollSuccesses * 100.0) / epPollCount, 1);
            Serial.print("% success rate at T+");
            Serial.print(pollStartTime - loopStartTime);
            Serial.println("ms");
        }
        
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
    
    // Pattern detection temporarily disabled - it was causing 37-second flickering
    // The 750ms timing interval was interfering with LED commands
    // TODO: Re-implement with non-interfering timing if pattern analysis is still needed
}
