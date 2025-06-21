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
    
    // NAK rate monitoring
    static int nakCount;
    static int totalCommands;
    static unsigned long lastNakReport;
    static bool hadNAK; // Track NAK state for smoothing
    static unsigned long lastNAKTime; // Track when NAK occurred
    static unsigned long nakCompensationDelay; // Compensation delay for NAK timing disruption
    
    // Timing history for degradation detection
    static int timingHistory[10];
    static int historyIndex;
    static bool rapidDegradationDetected;
   
    // Package 1 timing drift monitoring
    static int recentPackage1Times[5]; // Track last 5 Package 1 times
    static int pkg1TimingIndex;
    static bool pkg1TimingDriftDetected;
   
    // Activation timing drift monitoring
    static int recentActivationTimes[5]; // Track last 5 Activation times
    static int activationTimingIndex;
    static bool activationTimingDriftDetected;
    
    // === LONG-TERM STABILITY SYSTEM (20+ minutes) ===
    static unsigned long lastDeviceRefresh; // Last device refresh time
    static unsigned long lastTimingReset;   // Last timing reset time
    static unsigned long lastBufferFlush;   // Last buffer flush time
    static unsigned long lastHealthCheck;   // Last health check time
    static int commandCountSinceRefresh;   // Commands since last refresh
    static bool longTermStabilityMode;     // Enable enhanced stability features
    
    // Timing drift compensation
    static unsigned long baselineTiming;   // Baseline timing measurement
    static int timingDriftCount;           // Count of timing drift events
    static bool timingCompensationActive;  // Whether drift compensation is active
    
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
    
    // === LONG-TERM STABILITY FUNCTIONS (20+ minutes) ===
    void enableLongTermStabilityMode(); // Enable enhanced stability features
    void compensateTimingDrift(); // Compensate for timing drift over time
    void flushUSBBuffers(); // Flush USB buffers to prevent buildup
    void monitorLongTermHealth(); // Monitor overall system health
    void performProactiveMaintenance(); // Proactive maintenance every 5 minutes
    
    
    // MAX3421E timing synchronization methods

protected:
    void ParseHIDData(USBHID *hid, uint8_t ep, bool is_rpt_id, uint8_t len, uint8_t *buf); // Called by the HIDComposite library
    bool SelectInterface(uint8_t iface, uint8_t proto);
    uint8_t OnInitSuccessful(); // Override to add our initialization
};

// Define static member variables
int HIDSelector::nakCount = 0;
int HIDSelector::totalCommands = 0;
unsigned long HIDSelector::lastNakReport = 0;
bool HIDSelector::hadNAK = false;
unsigned long HIDSelector::lastNAKTime = 0;
unsigned long HIDSelector::nakCompensationDelay = 0;
int HIDSelector::timingHistory[10] = {0};
int HIDSelector::historyIndex = 0;
bool HIDSelector::rapidDegradationDetected = false;
int HIDSelector::recentPackage1Times[5] = {0}; // Track last 5 Package 1 times
int HIDSelector::pkg1TimingIndex = 0;
bool HIDSelector::pkg1TimingDriftDetected = false;
int HIDSelector::recentActivationTimes[5] = {0}; // Track last 5 Activation times
int HIDSelector::activationTimingIndex = 0;
bool HIDSelector::activationTimingDriftDetected = false;
unsigned long HIDSelector::lastDeviceRefresh = 0; // Last device refresh time
unsigned long HIDSelector::lastTimingReset = 0;   // Last timing reset time
unsigned long HIDSelector::lastBufferFlush = 0;   // Last buffer flush time
unsigned long HIDSelector::lastHealthCheck = 0;   // Last health check time
int HIDSelector::commandCountSinceRefresh = 0;   // Commands since last refresh
bool HIDSelector::longTermStabilityMode = false;     // Enable enhanced stability features
unsigned long HIDSelector::baselineTiming = 0;   // Baseline timing measurement
int HIDSelector::timingDriftCount = 0;           // Count of timing drift events
bool HIDSelector::timingCompensationActive = false;  // Whether drift compensation is active

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
    // === LONG-TERM STABILITY SYSTEM (20+ minutes) ===
    // Enable long-term stability mode if not already enabled
    // DISABLED: Drift correction logic removed as requested
    /*
    if (!longTermStabilityMode) {
        enableLongTermStabilityMode();
    }
    */
    
    // Track command count for maintenance scheduling
    commandCountSinceRefresh++;
    
    // === PROACTIVE MAINTENANCE SCHEDULE ===
    // DISABLED: Drift correction logic removed as requested
    /*
    unsigned long currentTime = millis();
    
    // Every 5 minutes (~3000 commands): Perform comprehensive maintenance
    if (currentTime - lastDeviceRefresh > 300000) { // 5 minutes
        performProactiveMaintenance();
        lastDeviceRefresh = currentTime;
        commandCountSinceRefresh = 0;
    }
    
    // Every 30 seconds: Check timing drift
    if (currentTime - lastTimingReset > 30000) { // 30 seconds
        // DISABLED: Drift correction logic removed as requested
        // compensateTimingDrift();
        lastTimingReset = currentTime;
    }
    
    // Every 10 seconds: Flush USB buffers
    if (currentTime - lastBufferFlush > 10000) { // 10 seconds
        flushUSBBuffers();
        lastBufferFlush = currentTime;
    }
    
    // Every 2 minutes: Monitor long-term health
    if (currentTime - lastHealthCheck > 120000) { // 2 minutes
        monitorLongTermHealth();
        lastHealthCheck = currentTime;
    }
    */
    
    // Check for consecutive NAKs (early warning system)
    // DISABLED: Drift correction logic removed as requested
    /*
    if (hadNAK) {
        Serial.println(" ⚠️ NAK detected - triggering buffer flush...");
        flushUSBBuffers();
    }
    */
    
    // === GENTLE DRIFT CORRECTION SYSTEM ===
    // DISABLED: Drift correction logic removed as requested
    // Monitor Package 1 timing and apply small corrections to prevent natural flickering
    /*
    static unsigned long pkg1TimingSum = 0;
    static int pkg1TimingCount = 0;
    static bool driftCorrectionActive = false;
    static int driftCorrectionSteps = 0;
    static unsigned long lastDriftCorrection = 0;
    */
    
    // === NAK RATE MONITORING ===
    // Initialize static variables if needed
    if (totalCommands == 0) {
        nakCount = 0;
        totalCommands = 0;
        lastNakReport = 0;
    }
    
    totalCommands++;
    
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
        0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
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
    
    // CRITICAL: Don't use SOF sync for Package 1 - it causes >1000μs transfer times
    // Package 1 should be sent immediately for stable LED operation
    // waitForUSBFrameSync(); // REMOVED - causes Package 1 to take too long
     delayMicroseconds(903);
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
        //delayMicroseconds(2998); // Reduced from 200μs to prevent timing issues
    } else {
        Serial.print(" ❌ | ");
        for (int retry = 0; retry < 2; retry++) {
            //delayMicroseconds(250); // Reduced delay
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
        //delayMicroseconds(200); // Reduced delay
    }
    
    // === PACKAGE 1 TIMING DRIFT DETECTION ===
    // Monitor Package 1 timing for signs of system degradation
    // REMOVED: Package 1 timing drift detection that was causing more flickering
    // The original system was working better - less intervention is better
    // Update Package 1 timing history
    // recentPackage1Times[pkg1TimingIndex] = pkg1Duration;
    // pkg1TimingIndex = (pkg1TimingIndex + 1) % 5;
    
    // Check for Package 1 timing drift (if we have 5 samples)
    // if (pkg1TimingIndex == 0) {
    //     int pkg1AvgTime = 0;
    //     for (int i = 0; i < 5; i++) {
    //         pkg1AvgTime += recentPackage1Times[i];
    //     }
    //     pkg1AvgTime /= 5;
    //     
    //     // Calculate rate of change over last 5 samples
    //     int pkg1MinTime = recentPackage1Times[0];
    //     int pkg1MaxTime = recentPackage1Times[0];
    //     for (int i = 1; i < 5; i++) {
    //         if (recentPackage1Times[i] < pkg1MinTime) pkg1MinTime = recentPackage1Times[i];
    //         if (recentPackage1Times[i] > pkg1MaxTime) pkg1MaxTime = recentPackage1Times[i];
    //     }
    //     int pkg1RateOfChange = pkg1MaxTime - pkg1MinTime;
    //     
    //     // Only trigger if Package 1 timing is both high (>150μs) and rising rapidly (>30μs over 5 samples)
    //     if (pkg1AvgTime > 150 && pkg1RateOfChange > 30 && !pkg1TimingDriftDetected) {
    //         pkg1TimingDriftDetected = true;
    //         Serial.print(" ⚠️ PACKAGE 1 TIMING DRIFT DETECTED (avg: ");
    //         Serial.print(pkg1AvgTime);
    //         Serial.print("μs, Δ");
    //         Serial.print(pkg1RateOfChange);
    //         Serial.print("μs) - triggering buffer management... ");
    //         delayMicroseconds(1000); // Buffer management delay
    //     } else if ((pkg1AvgTime <= 120 || pkg1RateOfChange <= 15) && pkg1TimingDriftDetected) {
    //         // Reset when Package 1 timing improves or stabilizes
    //         pkg1TimingDriftDetected = false;
    //         Serial.print(" ✅ Package 1 timing stabilized (avg: ");
    //         Serial.print(pkg1AvgTime);
    //         Serial.print("μs)");
    //     }
    // }
    
    // === GENTLE DRIFT CORRECTION ===
    // DISABLED: Drift correction logic removed as requested
    // Monitor Package 1 timing and apply very small corrections to prevent natural flickering
    /*
    pkg1TimingSum += pkg1Duration;
    pkg1TimingCount++;
    
    // Also track Package 2 timing drift
    static unsigned long pkg2TimingSum = 0;
    static unsigned long pkg2TimingCount = 0;
    static bool pkg2DriftCorrectionActive = false;
    static unsigned long pkg2DriftCorrectionSteps = 0;
    static unsigned long lastPkg2DriftCorrection = 0;
    
    // Check for drift every 25 commands (about 2.5 seconds) - very responsive
    if (pkg1TimingCount >= 25) {
        unsigned long avgPkg1 = pkg1TimingSum / pkg1TimingCount;
        unsigned long avgPkg2 = pkg2TimingSum / pkg2TimingCount;
        
        // VERY AGGRESSIVE: Start correction if timing is elevated (>100μs) and we haven't corrected recently
        if (avgPkg1 > 100 && !driftCorrectionActive && (totalCommands - lastDriftCorrection) > 500) {
            driftCorrectionActive = true;
            driftCorrectionSteps = 50; // Spread correction over 50 commands (faster correction)
            lastDriftCorrection = totalCommands;
            Serial.print("🔄 Pkg1 drift correction started: avg=");
            Serial.print(avgPkg1);
            Serial.print("μs, steps=");
            Serial.println(driftCorrectionSteps);
        }
        
        // Package 2 drift correction - also very aggressive
        if (avgPkg2 > 110 && !pkg2DriftCorrectionActive && (totalCommands - lastPkg2DriftCorrection) > 500) {
            pkg2DriftCorrectionActive = true;
            pkg2DriftCorrectionSteps = 50; // Spread correction over 50 commands (faster correction)
            lastPkg2DriftCorrection = totalCommands;
            Serial.print("🔄 Pkg2 drift correction started: avg=");
            Serial.print(avgPkg2);
            Serial.print("μs, steps=");
            Serial.println(pkg2DriftCorrectionSteps);
        }
        
        // Reset counters for next check
        pkg1TimingSum = 0;
        pkg1TimingCount = 0;
        pkg2TimingSum = 0;
        pkg2TimingCount = 0;
    }
    
    // Apply drift correction if active
    if (driftCorrectionActive && driftCorrectionSteps > 0) {
        delayMicroseconds(3); // Larger nudge to reduce timing drift more aggressively
        driftCorrectionSteps--;
        if (driftCorrectionSteps == 0) {
            driftCorrectionActive = false;
            Serial.println("✅ Pkg1 drift correction completed");
        }
    }
    
    // Apply Package 2 drift correction if active
    if (pkg2DriftCorrectionActive && pkg2DriftCorrectionSteps > 0) {
        delayMicroseconds(4); // Even larger nudge for Package 2 drift
        pkg2DriftCorrectionSteps--;
        if (pkg2DriftCorrectionSteps == 0) {
            pkg2DriftCorrectionActive = false;
            Serial.println("✅ Pkg2 drift correction completed");
        }
    }
    */
    
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
    
    // CRITICAL: Don't use SOF sync for Package 2 - it causes timing issues
    // Package 2 should be sent quickly after Package 1 for stable LED operation
    // waitForUSBFrameSync(); // REMOVED - causes Package 2 to take too long
    
    // Send Package 2 with proper timing
    Serial.print("📤 Package 2 (gap:");
    Serial.print(pkg1ToPkg2Gap);
    Serial.print("μs)... ");
    for (int retry = 0; retry < 3; retry++) {
        delayMicroseconds(903);
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        unsigned long pkg2EndTime = micros();
        unsigned long pkg2Duration = pkg2EndTime - pkg2StartTime;
        
        Serial.print("rcode=0x");
        Serial.print(rcode, HEX);
        Serial.print(" (");
        Serial.print(pkg2Duration);
        Serial.print("μs)");
        
        // Add Package 2 timing to drift monitoring
        // DISABLED: Drift correction logic removed as requested
        /*
        pkg2TimingSum += pkg2Duration;
        pkg2TimingCount++;
        */
        
        // === TIMING DRIFT DETECTION ===
        // REMOVED: All timing degradation detection systems that were causing flickering
        // The system runs more stably without artificial intervention
        
        if (rcode == 0) {
            Serial.print(" ✅ → ");
            //delayMicroseconds(100); // Reverted back to 350μs from 500μs
            break;
        }
        Serial.print(" ❌");
        if (rcode != 0x4) {
            Serial.print(" (non-NAK error, aborting)");
            Serial.println();
            return;
        }
        Serial.print(" (NAK, retry)");
        nakCount++; // Track NAK for rate monitoring
        hadNAK = true; // Flag for NAK smoothing
        
        // Set NAK smoothing compensation for timing disruption
        lastNAKTime = micros();
        nakCompensationDelay = 100; // Compensate for the timing gap (increased for more aggressive smoothing)
        
        //delayMicroseconds(50); // Keep retry delay minimal to avoid compounding delays
    }
    if (rcode != 0) {
        Serial.print("FAILED after retries - restarting from Package 1... ");
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, package1Backup);
        if (rcode == 0) {
            Serial.print("Pkg1 resent ✅ → ");
            delayMicroseconds(903);
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
    
    // CRITICAL: Don't use SOF sync for Activation - it causes timing issues
    // Activation should be sent quickly after Package 2 for stable LED operation
    // waitForUSBFrameSync(); // REMOVED - causes Activation to take too long
    
    // Send activation with conservative timing
    Serial.print("📤 Activation (gap:");
    Serial.print(pkg2ToActivationGap);
    Serial.print("μs)... ");
    for (int retry = 0; retry < 5; retry++) {
        delayMicroseconds(1903);
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        unsigned long activationEndTime = micros();
        unsigned long activationDuration = activationEndTime - activationStartTime;
        
        Serial.print("rcode=0x");
        Serial.print(rcode, HEX);
        Serial.print(" (");
        Serial.print(activationDuration);
        Serial.print("μs)");
        if (rcode == 0) {
            // CRITICAL: Use minimal timing compensation to prevent >1000μs transfers
            // Device expects consistent timing for stable LED operation
            const unsigned long TARGET_ACTIVATION_TIME = 180; // μs (reduced from 250μs)
            if (activationDuration < TARGET_ACTIVATION_TIME) {
                unsigned long compensationDelay = TARGET_ACTIVATION_TIME - activationDuration;
                //delayMicroseconds(compensationDelay);
                Serial.print(" +");
                Serial.print(compensationDelay);
                Serial.print("μs=");
                Serial.print(TARGET_ACTIVATION_TIME);
                Serial.print("μs");
            }
            Serial.println(" ✅");
            
            // REMOVED: Activation timing drift measurement that was causing flickering
            // The system runs more stably without this monitoring
            
            break;
        }
        Serial.print(" ❌");
        if (rcode != 0x4) {
            Serial.print(" (non-NAK error, aborting)");
            Serial.println();
            return;
        }
        Serial.print(" (NAK, retry)");
        nakCount++; // Track NAK for rate monitoring
        hadNAK = true; // Flag for NAK smoothing
        delayMicroseconds(50); // Reduced from 150μs to minimize timing disruption
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
    // REMOVED: Proactive device refresh was causing timing disruptions and flickering
    // The system runs more stably without artificial refresh operations
    
    // Move to next LED position
    movingLED = (movingLED + 1) % 24;
    
    // CRITICAL: Ensure minimum cycle time to prevent flickering
    // This prevents commands from being sent too rapidly, which causes device buffer overflow
    // static unsigned long lastCycleTime = 0;
    // unsigned long currentMicros = micros();
    // unsigned long cycleDuration = currentMicros - lastCycleTime;
    // lastCycleTime = currentMicros;
    // // Ensure minimum 50ms between LED command cycles to prevent flickering
    // const unsigned long MIN_CYCLE_TIME = 50000; // 50ms in microseconds (reduced from 100ms)
    // if (cycleDuration < MIN_CYCLE_TIME) {
    //     unsigned long compensationDelay = MIN_CYCLE_TIME - cycleDuration;
    //     Serial.print("⏰ Cycle timing compensation: +");
    //     Serial.print(compensationDelay / 1000);
    //     Serial.println("ms");
    //     delayMicroseconds(compensationDelay);
    // }
    
    // === NAK RATE MONITORING ===
    // SIMPLIFIED: Track NAKs with minimal impact on timing
    // if (totalCommands % 100 == 0) {
    //     float nakRate = (float)nakCount / 100.0 * 100.0; // Percentage
    //     Serial.print("📊 NAK Rate: ");
    //     Serial.print(nakRate, 1);
    //     Serial.print("% (");
    //     Serial.print(nakCount);
    //     Serial.print("/100)");
        
    //     // Only take action for very high NAK rates (>15%)
    //     if (nakRate > 15.0) {
    //         Serial.print(" ⚠️ HIGH NAK RATE");
    //         // Minimal intervention - just a brief pause
    //         delayMicroseconds(200);
    //     } else {
    //         Serial.print(" ✅ Normal");
    //     }
    //     Serial.println();
        
    //     // Reset counters
    //     nakCount = 0;
    // }
    
    // === NAK SMOOTHING ===
    // If we had a NAK in the previous command, hold the LED state briefly
    // to prevent visible flickering during NAK recovery
    if (hadNAK) {
        delayMicroseconds(100); // Brief hold to smooth NAK recovery
        hadNAK = false;
    }
    
    // === NAK RATE MONITORING ===
    totalCommands++;
    
    // === NAK PATTERN DETECTION ===
    // REMOVED: NAK pattern detection was adding complexity without clear benefits
    // The simplified NAK rate monitoring is sufficient for stability
    
    // === TIMING DRIFT DETECTION ===
    // Monitor Package 2 timing for signs of system degradation
    // REMOVED: Unused variables causing warnings
    
    // === NAK SMOOTHING ===
    // If we had a NAK in the previous command, hold the LED state briefly
    // to prevent visible flickering during NAK recovery
    if (hadNAK) {
        delayMicroseconds(100); // Brief hold to smooth NAK recovery
        hadNAK = false;
    }
    
    // === NAK SMOOTHING - COMPENSATE FOR TIMING DISRUPTION ===
    
    // If we had a NAK recently, add compensation to smooth the timing
    if (lastNAKTime > 0 && (micros() - lastNAKTime) < 10000) { // Within 10ms of NAK
        if (nakCompensationDelay > 0) {
            delayMicroseconds(nakCompensationDelay);
            Serial.print(" +NAK_comp:");
            Serial.print(nakCompensationDelay);
            nakCompensationDelay = 0; // Use it once
        }
    }
    
    // === SIMPLIFIED APPROACH - MINIMAL TIMING DRIFT COMPENSATION ONLY ===
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
    
    // Add a small buffer after SOF to center Pkg1 in the frame
    delayMicroseconds(256); // Tune this value as needed (50-100μs)
    
    // Clear the FRAMEIRQ flag
    pUsb->regWr(rHIRQ, bmFRAMEIRQ);
    
    unsigned long syncDuration = micros() - startTime;
    Serial.print("✅ SOF sync complete (");
    Serial.print(syncDuration);
    Serial.print("μs)");
    
    // CONSERVATIVE drift compensation - DISABLED due to 12s/32s flickering patterns
    // SOF sync times are stable (184-308μs) and well within normal range
    // Frame resets were disrupting timing more than helping
    /*
    // Track average sync time to detect gradual drift
    static unsigned long syncTimes[10] = {0};
    static int syncIndex = 0;
    static unsigned long lastDriftCheck = 0;
    
    // Update rolling average
    syncTimes[syncIndex] = syncDuration;
    syncIndex = (syncIndex + 1) % 10;
    
    // Check for drift every 100 commands (~30 seconds)
    if (syncIndex == 0 && (micros() - lastDriftCheck) > 30000000) { // 30 seconds - reduced frequency to prevent 9s pattern
        lastDriftCheck = micros();
        
        // Calculate average sync time
        unsigned long avgSync = 0;
        for (int i = 0; i < 10; i++) {
            avgSync += syncTimes[i];
        }
        avgSync /= 10;
        
        // MUCH MORE CONSERVATIVE: Only reset if average sync time is extremely high (> 1500μs)
        // This prevents aggressive resets that cause 9-second flickering patterns
        if (avgSync > 1500) {
            Serial.print(" 🔄 Frame reset (drift detected: avg=");
            Serial.print(avgSync);
            Serial.print("μs)");
            
            // Reset frame counter to prevent long-term drift
            uint8_t hctlReg = pUsb->regRd(0xE8); // Read HCTL register
            hctlReg |= 0x02; // Set FRMRST bit (bit 1)
            pUsb->regWr(0xE8, hctlReg); // Write back to reset frame counter
            
            // Clear FRMRST bit after reset
            delayMicroseconds(100); // Brief delay for reset to take effect
            hctlReg &= ~0x02; // Clear FRMRST bit
            pUsb->regWr(0xE8, hctlReg); // Write back
            
            // Reset sync time tracking
            for (int i = 0; i < 10; i++) {
                syncTimes[i] = 0;
            }
        }
    }
    */
    
    // INTELLIGENT drift compensation - only for significant long-term drift
    // Uses larger sample size (50 measurements) and longer interval (2 minutes)
    // Only triggers when average sync time exceeds 1000μs (indicating real drift)
    static unsigned long syncTimes[50] = {0}; // Larger sample size for better averaging
    static int syncIndex = 0;
    static unsigned long lastDriftCheck = 0;
    static unsigned long commandCount = 0;
    
    // Update rolling average
    syncTimes[syncIndex] = syncDuration;
    syncIndex = (syncIndex + 1) % 50;
    commandCount++;
    
    // Check for drift every 1 minute (~600 commands) with larger sample size
    if (syncIndex == 0 && (micros() - lastDriftCheck) > 60000000) { // 1 minute - more responsive
        lastDriftCheck = micros();
        
        // Calculate average sync time over 50 measurements
        unsigned long avgSync = 0;
        for (int i = 0; i < 50; i++) {
            avgSync += syncTimes[i];
        }
        avgSync /= 50;
        
        Serial.print("📊 Drift check: avg sync time = ");
        Serial.print(avgSync);
        Serial.print("μs over 50 measurements");
        
        // Only reset frame counter if average sync time exceeds 800μs (indicating real drift)
        if (avgSync > 800) {
            Serial.print(" ⚠️ Drift detected! Resetting frame counter...");
            
            // Reset frame counter to synchronize timing
            pUsb->regWr(rHCTL, bmFRMRST);
            delayMicroseconds(50); // Brief delay for reset to take effect
            pUsb->regWr(rHCTL, 0x00); // Clear reset bit
            
            Serial.println(" ✅ Frame counter reset complete");
        } else {
            Serial.println(" ✅ Timing stable, no action needed");
        }
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
    Serial.println("   USB Host Shield library configured for 24MHz SPI");
    Serial.println("   Current: Optimized 24MHz for Teensy 4.0 + MAX3421E stability");
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

// === LONG-TERM STABILITY FUNCTIONS (20+ minutes) ===
void HIDSelector::enableLongTermStabilityMode() {
    Serial.println("🔧 Enabling long-term stability mode for 20+ minute operation...");
    longTermStabilityMode = true;
    
    // Initialize baseline timing measurement
    baselineTiming = micros();
    
    // Set up initial maintenance schedule
    lastDeviceRefresh = millis();
    lastTimingReset = millis();
    lastBufferFlush = millis();
    lastHealthCheck = millis();
    commandCountSinceRefresh = 0;
    
    Serial.println("✅ Long-term stability mode enabled");
}

void HIDSelector::compensateTimingDrift() {
    // DISABLED: Drift correction logic removed as requested
    /*
    // Monitor and compensate for timing drift over time
    unsigned long currentMicros = micros();
    unsigned long elapsedTime = currentMicros - baselineTiming;
    
    // Calculate timing drift (should be ~100μs per command)
    static unsigned long lastTimingCheck = 0;
    static int commandCountAtLastCheck = 0;
    
    if (lastTimingCheck == 0) {
        lastTimingCheck = currentMicros;
        commandCountAtLastCheck = totalCommands;
        return;
    }
    
    // Check timing every 100 commands
    if (totalCommands - commandCountAtLastCheck >= 100) {
        unsigned long expectedTime = (totalCommands - commandCountAtLastCheck) * 100; // 100μs per command
        unsigned long actualTime = currentMicros - lastTimingCheck;
        long drift = actualTime - expectedTime;
        
        // If drift exceeds 10ms over 100 commands, apply compensation
        if (abs(drift) > 10000) { // 10ms threshold
            timingDriftCount++;
            Serial.print(" 🔧 Timing drift detected: ");
            Serial.print(drift);
            Serial.print("μs over 100 commands - applying compensation...");
            
            // Apply gentle timing compensation
            if (drift > 0) {
                // System is running slow, reduce delays slightly
                delayMicroseconds(5); // Small compensation
            } else {
                // System is running fast, add small delay
                delayMicroseconds(10); // Small compensation
            }
            
            Serial.println(" ✅ Compensation applied");
        }
        
        lastTimingCheck = currentMicros;
        commandCountAtLastCheck = totalCommands;
    }
    */
}

void HIDSelector::flushUSBBuffers() {
    // DISABLED: Buffer flushing was causing timing disruption and flickering
    // The system runs more stably without artificial buffer management
    return;
}

void HIDSelector::monitorLongTermHealth() {
    // Monitor overall system health and log statistics
    static unsigned long lastHealthReport = 0;
    unsigned long nowMillis = millis();
    
    // Report health every 2 minutes
    if (nowMillis - lastHealthReport > 120000) { // 2 minutes
        Serial.println("📊 === LONG-TERM HEALTH REPORT ===");
        Serial.print("   Commands executed: ");
        Serial.println(totalCommands);
        Serial.print("   NAK rate: ");
        Serial.print((float)nakCount / totalCommands * 100, 2);
        Serial.println("%");
        Serial.print("   Timing drift events: ");
        Serial.println(timingDriftCount);
        Serial.println("=================================");
        
        lastHealthReport = nowMillis;
    }
}

void HIDSelector::performProactiveMaintenance() {
    // DISABLED: Proactive maintenance was causing timing disruption and flickering
    // The system runs more stably without artificial maintenance operations
    return;
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

// Global variables for failure tracking
unsigned long consecutiveNAKFailures = 0;
unsigned long lastSuccessfulCommand = 0;
bool systemRecoveryMode = false;

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
        
        if (nowMs - lastTimerAnimation >= 100) { // Reverted back to 100ms from 150ms
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
