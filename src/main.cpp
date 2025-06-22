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
    
    // === OPTIMIZED FOR MIDI LOOPER - MINIMAL MEMORY FOOTPRINT ===
    // Remove all timing history arrays and monitoring systems
    // Keep only essential state tracking
    
    // Essential state only
    static bool deviceReady;
    static unsigned long lastCommandTime;
    static int commandCount;
    
    // === DMA-PROTECTED COMMAND BUFFERS ===
    // Replace stack-based cmd[64] with DMA-protected static buffers
    // This prevents memory corruption from ParseHIDData interrupts
    static uint8_t DMAMEM dmaSetCustom[64] __attribute__((aligned(32)));
    static uint8_t DMAMEM dmaPackage1[64] __attribute__((aligned(32)));
    static uint8_t DMAMEM dmaPackage2[64] __attribute__((aligned(32)));
    static uint8_t DMAMEM dmaCmd4180[64] __attribute__((aligned(32)));
    static uint8_t DMAMEM dmaActivation[64] __attribute__((aligned(32)));
    static uint8_t DMAMEM dmaPackage1Backup[64] __attribute__((aligned(32))); // Backup for corruption recovery
    
    // Pre-built command templates (const to save RAM)
    static const uint8_t PROGMEM setCustomTemplate[64];
    static const uint8_t PROGMEM package1Template[64];
    static const uint8_t PROGMEM package2Template[64];
    static const uint8_t PROGMEM cmd4180Template[64];
    static const uint8_t PROGMEM activationTemplate[64];
    
    // LED State Machine
    enum LedState {
        LED_IDLE,
        LED_SEND_PKG1,
        LED_SEND_PKG2,
        LED_SEND_ACTIVATE,
        LED_WAIT_ECHO
    };
    
    LedState ledState = LED_IDLE;
    uint32_t sofCounter = 0;
    uint8_t retryCount = 0;
    
public:
    HIDSelector(USB *p) : HIDComposite(p) {};
    
    // Streamlined interface for MIDI looper
    bool initializeInterface1();
    bool sendInitializationSequence();
    void testLEDCommands(); // Optimized version
    bool waitForPacketEcho(const uint8_t* sentPacket, const char* packetName);
    
    // Non-blocking LED state machine
    void handleLEDStateMachine();
    bool sendPacket(uint8_t* packet);
    bool checkEcho();
    
    // CRITICAL: Disable automatic polling that causes periodic flickering
    void disableAutomaticPolling() {
        pollInterval = 0; // Disable automatic polling completely
    }
    
    // Endpoint state management
    enum EndpointState {
        STATE_LED_COMMAND,    // Sending 0x04 LED commands
        STATE_POLL_0x83,      // Polling 0x83 for button data
        STATE_POLL_0x82,      // Polling 0x82 for status data
        STATE_WAIT_SETTLE     // Waiting between operations
    };
    
    EndpointState currentState = STATE_LED_COMMAND;
    elapsedMicros stateTimer;
    uint8_t stateCycle = 0;
    
    // State-managed endpoint polling
    void manageEndpoints() {
        // DISABLED: Endpoint management was causing 16-second flickering cycles
        // The 100ms polling intervals were interfering with LED command timing
        // Even with reduced frequency, EP polling creates cumulative timing drift
        return;
        
        switch (currentState) {
            case STATE_LED_COMMAND:
                if (stateTimer >= 50000) { // 50ms for LED commands
                    currentState = STATE_POLL_0x83;
                    stateTimer = 0;
                }
                break;
                
            case STATE_POLL_0x83:
                pollEndpoint0x83();
                currentState = STATE_POLL_0x82;
                stateTimer = 0;
                break;
                
            case STATE_POLL_0x82:
                pollEndpoint0x82();
                currentState = STATE_WAIT_SETTLE;
                stateTimer = 0;
                break;
                
            case STATE_WAIT_SETTLE:
                if (stateTimer >= 50000) { // 50ms settle time
                    currentState = STATE_LED_COMMAND;
                    stateTimer = 0;
                    stateCycle++;
                }
                break;
        }
    }
    
    // Poll 0x83 endpoint for button data
    void pollEndpoint0x83() {
        // DISABLED: EP 0x83 polling was causing systematic timing interference
        // Even optimized polling creates cumulative drift that manifests as flickering
        return;
        
        uint8_t buf[8];
        uint16_t len = 8;
        uint8_t rcode = pUsb->inTransfer(bAddress, 0x83, &len, buf);
        
        if (rcode == 0 && len > 0) {
            // Process button data if needed
            bool hasData = false;
            for (uint8_t i = 0; i < len; i++) {
                if (buf[i] != 0) {
                    hasData = true;
                    break;
                }
            }
            
            if (hasData) {
                Serial.print("📱 Button data (0x83): ");
                for (uint8_t i = 0; i < len; i++) {
                    if (buf[i] < 0x10) Serial.print("0");
                    Serial.print(buf[i], HEX);
                    Serial.print(" ");
                }
                Serial.println();
            }
        }
    }
    
    // Poll 0x82 endpoint for status data
    void pollEndpoint0x82() {
        // DISABLED: EP 0x82 polling was causing systematic timing interference
        // Even optimized polling creates cumulative drift that manifests as flickering
        return;
        
        uint8_t buf[8];
        uint16_t len = 8;
        uint8_t rcode = pUsb->inTransfer(bAddress, 0x82, &len, buf);
        
        if (rcode == 0 && len > 0) {
            // Process status data if needed
            bool hasData = false;
            for (uint8_t i = 0; i < len; i++) {
                if (buf[i] != 0) {
                    hasData = true;
                    break;
                }
            }
            
            if (hasData) {
                Serial.print("📊 Status data (0x82): ");
                for (uint8_t i = 0; i < len; i++) {
                    if (buf[i] < 0x10) Serial.print("0");
                    Serial.print(buf[i], HEX);
                    Serial.print(" ");
                }
                Serial.println();
            }
        }
    }
    
    // Legacy method for compatibility
    bool checkButtonState() {
        // Placeholder for button state checking
        // Can be implemented if button monitoring is needed
        return false;
    }
    
    // Remove all timing compensation and monitoring functions
    // Keep only essential device communication

protected:
    void ParseHIDData(USBHID *hid, uint8_t ep, bool is_rpt_id, uint8_t len, uint8_t *buf);
    bool SelectInterface(uint8_t iface, uint8_t proto);
    uint8_t OnInitSuccessful();
};

// Define static member variables
bool HIDSelector::deviceReady = false;
unsigned long HIDSelector::lastCommandTime = 0;
int HIDSelector::commandCount = 0;

// === DMA-PROTECTED COMMAND BUFFERS ===
uint8_t DMAMEM HIDSelector::dmaSetCustom[64] __attribute__((aligned(32)));
uint8_t DMAMEM HIDSelector::dmaPackage1[64] __attribute__((aligned(32)));
uint8_t DMAMEM HIDSelector::dmaPackage2[64] __attribute__((aligned(32)));
uint8_t DMAMEM HIDSelector::dmaCmd4180[64] __attribute__((aligned(32)));
uint8_t DMAMEM HIDSelector::dmaActivation[64] __attribute__((aligned(32)));
uint8_t DMAMEM HIDSelector::dmaPackage1Backup[64] __attribute__((aligned(32)));

// Pre-built command templates in PROGMEM to save RAM
const uint8_t PROGMEM HIDSelector::setCustomTemplate[64] = {
    0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t PROGMEM HIDSelector::package1Template[64] = {
    0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x80, 0x01, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t PROGMEM HIDSelector::package2Template[64] = {
    0x56, 0x83, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t PROGMEM HIDSelector::cmd4180Template[64] = {
    0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t PROGMEM HIDSelector::activationTemplate[64] = {
    0x51, 0x28, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Rainbow colors for LED animation
static const uint8_t PROGMEM rainbowColors[7][3] = {
    {0xFF, 0x00, 0x00}, // Red
    {0xFF, 0x80, 0x00}, // Orange
    {0xFF, 0xFF, 0x00}, // Yellow
    {0x00, 0xFF, 0x00}, // Green
    {0x00, 0xFF, 0xFF}, // Cyan
    {0x00, 0x00, 0xFF}, // Blue
    {0xFF, 0x00, 0xFF}  // Magenta
};

// Animation state
static int currentColorIndex = 0;

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
        // DISABLED: Test command that was causing initial NAKs
        // Serial.println("⏳ Allowing device to settle after initialization...");
        // delay(250); // 250ms settling time after initialization before first LED command
        // Serial.println("🌈 Testing LED command sequence...");
        // testLEDCommands();
        Serial.println("🎯 Device ready for LED animation - skipping test command for clean startup");
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
    
    // Enable SOF frame generation for proper device timing, but keep FRAMEIRQ disabled
    Serial.println("📡 Enabling SOF frame generation (SOFKAENAB) for device timing...");
    uint8_t modeReg = pUsb->regRd(0x27); // Read current MODE register
    modeReg |= 0x08; // Set SOFKAENAB bit (bit 3) - corrected bit position per datasheet
    pUsb->regWr(0x27, modeReg); // Write back with SOFKAENAB enabled
    
    // KEEP FRAMEIRQ DISABLED - interrupts cause timing disruption for MIDI applications
    // uint8_t hienReg = pUsb->regRd(0x26); // Read current HIEN register  
    // hienReg |= 0x40; // Set FRAMEIE bit (bit 6)
    // pUsb->regWr(0x26, hienReg); // Write back with FRAMEIE enabled
    
    Serial.println("✅ SOF generation ENABLED, FRAMEIRQ DISABLED for optimal MIDI timing");
    delay(50);
    
    Serial.println("✅ Initialization sequence complete");
    return true;
}

// Will be called for all HID data received from the USB interface
void HIDSelector::ParseHIDData(USBHID *hid, uint8_t ep, bool is_rpt_id, uint8_t len, uint8_t *buf) {
    if (len && buf) {
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

// Removed unused toggle management functions for MIDI looper optimization

// Test LED command sequence - OPTIMIZED FOR MIDI LOOPER
void HIDSelector::testLEDCommands() {
    // === INTERRUPT PROTECTION FOR CRITICAL BUFFER OPERATIONS ===
    __disable_irq(); // Disable interrupts during buffer preparation
    
    // === USE DMA-PROTECTED BUFFERS INSTEAD OF STACK ===
    // Copy templates from PROGMEM to DMA-protected buffers
    memcpy_P(dmaPackage1, package1Template, 64);
    memcpy_P(dmaPackage2, package2Template, 64);
    memcpy_P(dmaActivation, activationTemplate, 64);
    
    // Backup Package 1 for corruption detection
    memcpy(dmaPackage1Backup, dmaPackage1, 64);
    
    __enable_irq(); // Re-enable interrupts
    
    // === OPTIMIZED RAINBOW COLORS (PROGMEM) ===
    static const uint8_t PROGMEM rainbowColors[24][3] = {
        {0xFF, 0x00, 0x00}, {0xFF, 0x80, 0x00}, {0xFF, 0xFF, 0x00}, {0x80, 0xFF, 0x00}, // 1,2,3,4
        {0x00, 0xFF, 0x00}, {0x00, 0xFF, 0x80}, {0x00, 0xFF, 0xFF}, {0x00, 0x80, 0xFF}, // 5,6,7,8
        {0x00, 0x00, 0xFF}, {0x80, 0x00, 0xFF}, {0xFF, 0x00, 0xFF}, {0xFF, 0x00, 0x80}, // 9,10,11,12
        {0xFF, 0x40, 0x40}, {0x40, 0xFF, 0x40}, {0x40, 0x40, 0xFF}, {0xFF, 0xFF, 0x40}, // 13,14,15,16
        {0xFF, 0x40, 0xFF}, {0x40, 0xFF, 0xFF}, {0x80, 0x80, 0x80}, {0x60, 0x60, 0x60}, // 17,18,19,20
        {0x40, 0x40, 0x40}, {0x20, 0x20, 0x20}, {0xC0, 0xC0, 0xC0}, {0xA0, 0xA0, 0xA0}  // 21,22,23,24
    };
    
    // === FAST ANIMATION UPDATE ===
    static int movingLED = 0;
    static elapsedMicros ledMoveTimer;
    
    if (ledMoveTimer >= 100000) {
        movingLED = (movingLED + 1) % 24;
        ledMoveTimer = 0;
    }
    
    // === FAST RGB DATA ASSIGNMENT USING DIRECT POINTER ACCESS ===
    __disable_irq(); // Protect buffer during RGB updates
    
    // === PACKAGE 1: Complete RGB data assignment ===
    // Package 1 RGB data starts at byte 24 according to the working structure
    // Following the exact mapping from "breakdown of 5 commands to set 24 leds.txt":
    
    // buttons 1,6,11,16,21 (indices 0,5,10,15,20)
    dmaPackage1[24] = (movingLED == 0) ? 0xFF : pgm_read_byte(&rainbowColors[0][0]);   // button 1 R
    dmaPackage1[25] = (movingLED == 0) ? 0xFF : pgm_read_byte(&rainbowColors[0][1]);   // button 1 G  
    dmaPackage1[26] = (movingLED == 0) ? 0xFF : pgm_read_byte(&rainbowColors[0][2]);   // button 1 B
    
    dmaPackage1[27] = (movingLED == 5) ? 0xFF : pgm_read_byte(&rainbowColors[5][0]);   // button 6 R
    dmaPackage1[28] = (movingLED == 5) ? 0xFF : pgm_read_byte(&rainbowColors[5][1]);   // button 6 G
    dmaPackage1[29] = (movingLED == 5) ? 0xFF : pgm_read_byte(&rainbowColors[5][2]);   // button 6 B
    
    dmaPackage1[30] = (movingLED == 10) ? 0xFF : pgm_read_byte(&rainbowColors[10][0]); // button 11 R
    dmaPackage1[31] = (movingLED == 10) ? 0xFF : pgm_read_byte(&rainbowColors[10][1]); // button 11 G
    dmaPackage1[32] = (movingLED == 10) ? 0xFF : pgm_read_byte(&rainbowColors[10][2]); // button 11 B
    
    dmaPackage1[33] = (movingLED == 15) ? 0xFF : pgm_read_byte(&rainbowColors[15][0]); // button 16 R
    dmaPackage1[34] = (movingLED == 15) ? 0xFF : pgm_read_byte(&rainbowColors[15][1]); // button 16 G
    dmaPackage1[35] = (movingLED == 15) ? 0xFF : pgm_read_byte(&rainbowColors[15][2]); // button 16 B
    
    dmaPackage1[36] = (movingLED == 20) ? 0xFF : pgm_read_byte(&rainbowColors[20][0]); // button 21 R
    dmaPackage1[37] = (movingLED == 20) ? 0xFF : pgm_read_byte(&rainbowColors[20][1]); // button 21 G
    dmaPackage1[38] = (movingLED == 20) ? 0xFF : pgm_read_byte(&rainbowColors[20][2]); // button 21 B
    
    // buttons 2,7,12,17,22 (indices 1,6,11,16,21)
    dmaPackage1[39] = (movingLED == 1) ? 0xFF : pgm_read_byte(&rainbowColors[1][0]);   // button 2 R
    dmaPackage1[40] = (movingLED == 1) ? 0xFF : pgm_read_byte(&rainbowColors[1][1]);   // button 2 G
    dmaPackage1[41] = (movingLED == 1) ? 0xFF : pgm_read_byte(&rainbowColors[1][2]);   // button 2 B
    
    dmaPackage1[42] = (movingLED == 6) ? 0xFF : pgm_read_byte(&rainbowColors[6][0]);   // button 7 R
    dmaPackage1[43] = (movingLED == 6) ? 0xFF : pgm_read_byte(&rainbowColors[6][1]);   // button 7 G
    dmaPackage1[44] = (movingLED == 6) ? 0xFF : pgm_read_byte(&rainbowColors[6][2]);   // button 7 B
    
    dmaPackage1[45] = (movingLED == 11) ? 0xFF : pgm_read_byte(&rainbowColors[11][0]); // button 12 R
    dmaPackage1[46] = (movingLED == 11) ? 0xFF : pgm_read_byte(&rainbowColors[11][1]); // button 12 G
    dmaPackage1[47] = (movingLED == 11) ? 0xFF : pgm_read_byte(&rainbowColors[11][2]); // button 12 B
    
    dmaPackage1[48] = (movingLED == 16) ? 0xFF : pgm_read_byte(&rainbowColors[16][0]); // button 17 R
    dmaPackage1[49] = (movingLED == 16) ? 0xFF : pgm_read_byte(&rainbowColors[16][1]); // button 17 G
    dmaPackage1[50] = (movingLED == 16) ? 0xFF : pgm_read_byte(&rainbowColors[16][2]); // button 17 B
    
    dmaPackage1[51] = (movingLED == 21) ? 0xFF : pgm_read_byte(&rainbowColors[21][0]); // button 22 R
    dmaPackage1[52] = (movingLED == 21) ? 0xFF : pgm_read_byte(&rainbowColors[21][1]); // button 22 G
    dmaPackage1[53] = (movingLED == 21) ? 0xFF : pgm_read_byte(&rainbowColors[21][2]); // button 22 B
    
    // buttons 3,8,13 (indices 2,7,12)
    dmaPackage1[54] = (movingLED == 2) ? 0xFF : pgm_read_byte(&rainbowColors[2][0]);   // button 3 R
    dmaPackage1[55] = (movingLED == 2) ? 0xFF : pgm_read_byte(&rainbowColors[2][1]);   // button 3 G
    dmaPackage1[56] = (movingLED == 2) ? 0xFF : pgm_read_byte(&rainbowColors[2][2]);   // button 3 B
    
    dmaPackage1[57] = (movingLED == 7) ? 0xFF : pgm_read_byte(&rainbowColors[7][0]);   // button 8 R
    dmaPackage1[58] = (movingLED == 7) ? 0xFF : pgm_read_byte(&rainbowColors[7][1]);   // button 8 G
    dmaPackage1[59] = (movingLED == 7) ? 0xFF : pgm_read_byte(&rainbowColors[7][2]);   // button 8 B
    
    dmaPackage1[60] = (movingLED == 12) ? 0xFF : pgm_read_byte(&rainbowColors[12][0]); // button 13 R
    dmaPackage1[61] = (movingLED == 12) ? 0xFF : pgm_read_byte(&rainbowColors[12][1]); // button 13 G
    dmaPackage1[62] = (movingLED == 12) ? 0xFF : pgm_read_byte(&rainbowColors[12][2]); // button 13 B
    
    // button 18 R component only (index 17)
    dmaPackage1[63] = (movingLED == 17) ? 0xFF : pgm_read_byte(&rainbowColors[17][0]); // button 18 R only
    
    // === PACKAGE 2: Complete RGB data assignment ===
    // Package 2: 56 83 01 00 [RGB continuation data]
    // The 0x00 at byte 3 is a masked R value as mentioned
    
    // Button 18 GB components (continuing from Package 1) - bytes 4-5 (after the masked 0x00)
    dmaPackage2[4] = (movingLED == 17) ? 0xFF : pgm_read_byte(&rainbowColors[17][1]); // button 18 G
    dmaPackage2[5] = (movingLED == 17) ? 0xFF : pgm_read_byte(&rainbowColors[17][2]); // button 18 B
    
    // Button 23 RGB - bytes 6-8
    dmaPackage2[6] = (movingLED == 22) ? 0xFF : pgm_read_byte(&rainbowColors[22][0]); // button 23 R
    dmaPackage2[7] = (movingLED == 22) ? 0xFF : pgm_read_byte(&rainbowColors[22][1]); // button 23 G
    dmaPackage2[8] = (movingLED == 22) ? 0xFF : pgm_read_byte(&rainbowColors[22][2]); // button 23 B
    
    // buttons 4,9,14,19,24 (indices 3,8,13,18,23) - bytes 9-23
    dmaPackage2[9] = (movingLED == 3) ? 0xFF : pgm_read_byte(&rainbowColors[3][0]);   // button 4 R
    dmaPackage2[10] = (movingLED == 3) ? 0xFF : pgm_read_byte(&rainbowColors[3][1]);  // button 4 G
    dmaPackage2[11] = (movingLED == 3) ? 0xFF : pgm_read_byte(&rainbowColors[3][2]);  // button 4 B
    
    dmaPackage2[12] = (movingLED == 8) ? 0xFF : pgm_read_byte(&rainbowColors[8][0]);  // button 9 R
    dmaPackage2[13] = (movingLED == 8) ? 0xFF : pgm_read_byte(&rainbowColors[8][1]);  // button 9 G
    dmaPackage2[14] = (movingLED == 8) ? 0xFF : pgm_read_byte(&rainbowColors[8][2]);  // button 9 B
    
    dmaPackage2[15] = (movingLED == 13) ? 0xFF : pgm_read_byte(&rainbowColors[13][0]); // button 14 R
    dmaPackage2[16] = (movingLED == 13) ? 0xFF : pgm_read_byte(&rainbowColors[13][1]); // button 14 G
    dmaPackage2[17] = (movingLED == 13) ? 0xFF : pgm_read_byte(&rainbowColors[13][2]); // button 14 B
    
    dmaPackage2[18] = (movingLED == 18) ? 0xFF : pgm_read_byte(&rainbowColors[18][0]); // button 19 R
    dmaPackage2[19] = (movingLED == 18) ? 0xFF : pgm_read_byte(&rainbowColors[18][1]); // button 19 G
    dmaPackage2[20] = (movingLED == 18) ? 0xFF : pgm_read_byte(&rainbowColors[18][2]); // button 19 B
    
    dmaPackage2[21] = (movingLED == 23) ? 0xFF : pgm_read_byte(&rainbowColors[23][0]); // button 24 R
    dmaPackage2[22] = (movingLED == 23) ? 0xFF : pgm_read_byte(&rainbowColors[23][1]); // button 24 G
    dmaPackage2[23] = (movingLED == 23) ? 0xFF : pgm_read_byte(&rainbowColors[23][2]); // button 24 B
    
    // buttons 5,10,15,20 (indices 4,9,14,19) - bytes 24-35
    dmaPackage2[24] = (movingLED == 4) ? 0xFF : pgm_read_byte(&rainbowColors[4][0]);  // button 5 R
    dmaPackage2[25] = (movingLED == 4) ? 0xFF : pgm_read_byte(&rainbowColors[4][1]);  // button 5 G
    dmaPackage2[26] = (movingLED == 4) ? 0xFF : pgm_read_byte(&rainbowColors[4][2]);  // button 5 B
    
    dmaPackage2[27] = (movingLED == 9) ? 0xFF : pgm_read_byte(&rainbowColors[9][0]);  // button 10 R
    dmaPackage2[28] = (movingLED == 9) ? 0xFF : pgm_read_byte(&rainbowColors[9][1]);  // button 10 G
    dmaPackage2[29] = (movingLED == 9) ? 0xFF : pgm_read_byte(&rainbowColors[9][2]);  // button 10 B
    
    dmaPackage2[30] = (movingLED == 14) ? 0xFF : pgm_read_byte(&rainbowColors[14][0]); // button 15 R
    dmaPackage2[31] = (movingLED == 14) ? 0xFF : pgm_read_byte(&rainbowColors[14][1]); // button 15 G
    dmaPackage2[32] = (movingLED == 14) ? 0xFF : pgm_read_byte(&rainbowColors[14][2]); // button 15 B
    
    dmaPackage2[33] = (movingLED == 19) ? 0xFF : pgm_read_byte(&rainbowColors[19][0]); // button 20 R
    dmaPackage2[34] = (movingLED == 19) ? 0xFF : pgm_read_byte(&rainbowColors[19][1]); // button 20 G
    dmaPackage2[35] = (movingLED == 19) ? 0xFF : pgm_read_byte(&rainbowColors[19][2]); // button 20 B
    
    __enable_irq(); // Re-enable interrupts
    
    // === ATOMIC SEND: no echo in between ===
    uint8_t rcode;

    // 1) Send Pkg1
    for (int retry = 0; retry < 3; retry++) {
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, dmaPackage1);
        if (rcode == 0) break;
        if (rcode == hrNAK) continue;
        Serial.print("❌ Pkg1 failed: 0x"); 
        Serial.println(rcode, HEX);
        return;
    }
    if (rcode != 0) return;

    // 2) Send Pkg2 immediately
    for (int retry = 0; retry < 3; retry++) {
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, dmaPackage2);
        if (rcode == 0) break;
        if (rcode == hrNAK) continue;
        Serial.print("❌ Pkg2 failed: 0x"); 
        Serial.println(rcode, HEX);
        return;
    }
    if (rcode != 0) return;

    // 3) Send Activation immediately
    for (int retry = 0; retry < 3; retry++) {
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, dmaActivation);
        if (rcode == 0) break;
        if (rcode == hrNAK) continue;
        Serial.print("❌ Activation failed: 0x"); 
        Serial.println(rcode, HEX);
        return;
    }
    if (rcode != 0) return;

    // 4) Skip echo wait to prevent command skipping
    // if (!waitForPacketEcho(dmaActivation, "Activation")) {
    //     Serial.println("❌ Activation: No echo");
    //     return;
    // }
    
    // === MINIMAL TRACKING FOR MIDI LOOPER ===
    commandCount++;
    lastCommandTime = millis();
}

// Wait for device echo to confirm packet was received and processed
// Device should echo back the first 3 bytes of any command sent
bool HIDSelector::waitForPacketEcho(const uint8_t* sentPacket, const char* packetName) {
    // MICROSECOND PRECISION: Use elapsedMicros for precise timing
    // Based on USB capture: device responds in 1.5-1.9ms (1500-1900 microseconds)
    
    elapsedMicros totalTime;
    elapsedMicros pollTime;
    
    // Poll for up to 5ms (5000 microseconds) total timeout
    while (totalTime < 6000) {
        // Only poll every 128μs for optimal response (power-of-2 aligned)
        if (pollTime >= 500) {
            pollTime = 0; // Reset poll timer
            
        uint16_t bytesRead = 64;
        uint8_t buffer[64];
        uint8_t rcode = pUsb->inTransfer(bAddress, 0x03, &bytesRead, buffer);
        
        if (rcode == 0 && bytesRead > 0) {
            // Check for echo in (Package 1): 56 83 00 ...
            
            bool foundEcho = false;
            
            // Check direct format (bytes 0-2)
            if (bytesRead >= 3 && 
                buffer[0] == sentPacket[0] && 
                buffer[1] == sentPacket[1] && 
                buffer[2] == sentPacket[2]) {
                foundEcho = true;
            }
            
            if (foundEcho) {
                return true; // Found expected echo
            }
        }
        }
    }
    
    // Timeout - no echo received after 5ms
    return false;
}

// Non-blocking LED state machine - handles one state per SOF frame
void HIDSelector::handleLEDStateMachine() {
    // === FAST ANIMATION UPDATE ===
    static int movingLED = 0;
    static uint32_t ledMoveCounter = 0;
    
    // Move LED every ~100ms (100 SOF frames)
    ledMoveCounter++;
    if (ledMoveCounter >= 100) {
        movingLED = (movingLED + 1) % 24;
        ledMoveCounter = 0;
    }
    
    sofCounter++;
    
    switch (ledState) {
        case LED_IDLE:
            if (sofCounter >= 40) {  // ~40ms update rate
                sofCounter = 0;
                retryCount = 0;
                
                // Prepare RGB data
                __disable_irq();
                memcpy_P(dmaPackage1, package1Template, 64);
                memcpy_P(dmaPackage2, package2Template, 64);
                memcpy_P(dmaActivation, activationTemplate, 64);
                
                // === OPTIMIZED RAINBOW COLORS (PROGMEM) ===
                static const uint8_t PROGMEM rainbowColors[24][3] = {
                    {0xFF, 0x00, 0x00}, {0xFF, 0x80, 0x00}, {0xFF, 0xFF, 0x00}, {0x80, 0xFF, 0x00},
                    {0x00, 0xFF, 0x00}, {0x00, 0xFF, 0x80}, {0x00, 0xFF, 0xFF}, {0x00, 0x80, 0xFF},
                    {0x00, 0x00, 0xFF}, {0x80, 0x00, 0xFF}, {0xFF, 0x00, 0xFF}, {0xFF, 0x00, 0x80},
                    {0xFF, 0x40, 0x40}, {0x40, 0xFF, 0x40}, {0x40, 0x40, 0xFF}, {0xFF, 0xFF, 0x40},
                    {0xFF, 0x40, 0xFF}, {0x40, 0xFF, 0xFF}, {0x80, 0x80, 0x80}, {0x60, 0x60, 0x60},
                    {0x40, 0x40, 0x40}, {0x20, 0x20, 0x20}, {0xC0, 0xC0, 0xC0}, {0xA0, 0xA0, 0xA0}
                };
                
                // Update RGB data (complete mapping from original testLEDCommands)
                // Package 1 RGB data starts at byte 24
                
                // buttons 1,6,11,16,21 (indices 0,5,10,15,20)
                dmaPackage1[24] = (movingLED == 0) ? 0xFF : pgm_read_byte(&rainbowColors[0][0]);
                dmaPackage1[25] = (movingLED == 0) ? 0xFF : pgm_read_byte(&rainbowColors[0][1]);
                dmaPackage1[26] = (movingLED == 0) ? 0xFF : pgm_read_byte(&rainbowColors[0][2]);
                
                dmaPackage1[27] = (movingLED == 5) ? 0xFF : pgm_read_byte(&rainbowColors[5][0]);
                dmaPackage1[28] = (movingLED == 5) ? 0xFF : pgm_read_byte(&rainbowColors[5][1]);
                dmaPackage1[29] = (movingLED == 5) ? 0xFF : pgm_read_byte(&rainbowColors[5][2]);
                
                dmaPackage1[30] = (movingLED == 10) ? 0xFF : pgm_read_byte(&rainbowColors[10][0]);
                dmaPackage1[31] = (movingLED == 10) ? 0xFF : pgm_read_byte(&rainbowColors[10][1]);
                dmaPackage1[32] = (movingLED == 10) ? 0xFF : pgm_read_byte(&rainbowColors[10][2]);
                
                dmaPackage1[33] = (movingLED == 15) ? 0xFF : pgm_read_byte(&rainbowColors[15][0]);
                dmaPackage1[34] = (movingLED == 15) ? 0xFF : pgm_read_byte(&rainbowColors[15][1]);
                dmaPackage1[35] = (movingLED == 15) ? 0xFF : pgm_read_byte(&rainbowColors[15][2]);
                
                dmaPackage1[36] = (movingLED == 20) ? 0xFF : pgm_read_byte(&rainbowColors[20][0]);
                dmaPackage1[37] = (movingLED == 20) ? 0xFF : pgm_read_byte(&rainbowColors[20][1]);
                dmaPackage1[38] = (movingLED == 20) ? 0xFF : pgm_read_byte(&rainbowColors[20][2]);
                
                // buttons 2,7,12,17,22 (indices 1,6,11,16,21)
                dmaPackage1[39] = (movingLED == 1) ? 0xFF : pgm_read_byte(&rainbowColors[1][0]);
                dmaPackage1[40] = (movingLED == 1) ? 0xFF : pgm_read_byte(&rainbowColors[1][1]);
                dmaPackage1[41] = (movingLED == 1) ? 0xFF : pgm_read_byte(&rainbowColors[1][2]);
                
                dmaPackage1[42] = (movingLED == 6) ? 0xFF : pgm_read_byte(&rainbowColors[6][0]);
                dmaPackage1[43] = (movingLED == 6) ? 0xFF : pgm_read_byte(&rainbowColors[6][1]);
                dmaPackage1[44] = (movingLED == 6) ? 0xFF : pgm_read_byte(&rainbowColors[6][2]);
                
                dmaPackage1[45] = (movingLED == 11) ? 0xFF : pgm_read_byte(&rainbowColors[11][0]);
                dmaPackage1[46] = (movingLED == 11) ? 0xFF : pgm_read_byte(&rainbowColors[11][1]);
                dmaPackage1[47] = (movingLED == 11) ? 0xFF : pgm_read_byte(&rainbowColors[11][2]);
                
                dmaPackage1[48] = (movingLED == 16) ? 0xFF : pgm_read_byte(&rainbowColors[16][0]);
                dmaPackage1[49] = (movingLED == 16) ? 0xFF : pgm_read_byte(&rainbowColors[16][1]);
                dmaPackage1[50] = (movingLED == 16) ? 0xFF : pgm_read_byte(&rainbowColors[16][2]);
                
                dmaPackage1[51] = (movingLED == 21) ? 0xFF : pgm_read_byte(&rainbowColors[21][0]);
                dmaPackage1[52] = (movingLED == 21) ? 0xFF : pgm_read_byte(&rainbowColors[21][1]);
                dmaPackage1[53] = (movingLED == 21) ? 0xFF : pgm_read_byte(&rainbowColors[21][2]);
                
                // buttons 3,8,13 (indices 2,7,12)
                dmaPackage1[54] = (movingLED == 2) ? 0xFF : pgm_read_byte(&rainbowColors[2][0]);
                dmaPackage1[55] = (movingLED == 2) ? 0xFF : pgm_read_byte(&rainbowColors[2][1]);
                dmaPackage1[56] = (movingLED == 2) ? 0xFF : pgm_read_byte(&rainbowColors[2][2]);
                
                dmaPackage1[57] = (movingLED == 7) ? 0xFF : pgm_read_byte(&rainbowColors[7][0]);
                dmaPackage1[58] = (movingLED == 7) ? 0xFF : pgm_read_byte(&rainbowColors[7][1]);
                dmaPackage1[59] = (movingLED == 7) ? 0xFF : pgm_read_byte(&rainbowColors[7][2]);
                
                dmaPackage1[60] = (movingLED == 12) ? 0xFF : pgm_read_byte(&rainbowColors[12][0]);
                dmaPackage1[61] = (movingLED == 12) ? 0xFF : pgm_read_byte(&rainbowColors[12][1]);
                dmaPackage1[62] = (movingLED == 12) ? 0xFF : pgm_read_byte(&rainbowColors[12][2]);
                
                // button 18 R component only (index 17)
                dmaPackage1[63] = (movingLED == 17) ? 0xFF : pgm_read_byte(&rainbowColors[17][0]);
                
                // Package 2 RGB data
                dmaPackage2[4] = (movingLED == 17) ? 0xFF : pgm_read_byte(&rainbowColors[17][1]);
                dmaPackage2[5] = (movingLED == 17) ? 0xFF : pgm_read_byte(&rainbowColors[17][2]);
                
                dmaPackage2[6] = (movingLED == 22) ? 0xFF : pgm_read_byte(&rainbowColors[22][0]);
                dmaPackage2[7] = (movingLED == 22) ? 0xFF : pgm_read_byte(&rainbowColors[22][1]);
                dmaPackage2[8] = (movingLED == 22) ? 0xFF : pgm_read_byte(&rainbowColors[22][2]);
                
                // buttons 4,9,14,19,24
                dmaPackage2[9] = (movingLED == 3) ? 0xFF : pgm_read_byte(&rainbowColors[3][0]);
                dmaPackage2[10] = (movingLED == 3) ? 0xFF : pgm_read_byte(&rainbowColors[3][1]);
                dmaPackage2[11] = (movingLED == 3) ? 0xFF : pgm_read_byte(&rainbowColors[3][2]);
                
                dmaPackage2[12] = (movingLED == 8) ? 0xFF : pgm_read_byte(&rainbowColors[8][0]);
                dmaPackage2[13] = (movingLED == 8) ? 0xFF : pgm_read_byte(&rainbowColors[8][1]);
                dmaPackage2[14] = (movingLED == 8) ? 0xFF : pgm_read_byte(&rainbowColors[8][2]);
                
                dmaPackage2[15] = (movingLED == 13) ? 0xFF : pgm_read_byte(&rainbowColors[13][0]);
                dmaPackage2[16] = (movingLED == 13) ? 0xFF : pgm_read_byte(&rainbowColors[13][1]);
                dmaPackage2[17] = (movingLED == 13) ? 0xFF : pgm_read_byte(&rainbowColors[13][2]);
                
                dmaPackage2[18] = (movingLED == 18) ? 0xFF : pgm_read_byte(&rainbowColors[18][0]);
                dmaPackage2[19] = (movingLED == 18) ? 0xFF : pgm_read_byte(&rainbowColors[18][1]);
                dmaPackage2[20] = (movingLED == 18) ? 0xFF : pgm_read_byte(&rainbowColors[18][2]);
                
                dmaPackage2[21] = (movingLED == 23) ? 0xFF : pgm_read_byte(&rainbowColors[23][0]);
                dmaPackage2[22] = (movingLED == 23) ? 0xFF : pgm_read_byte(&rainbowColors[23][1]);
                dmaPackage2[23] = (movingLED == 23) ? 0xFF : pgm_read_byte(&rainbowColors[23][2]);
                
                // buttons 5,10,15,20
                dmaPackage2[24] = (movingLED == 4) ? 0xFF : pgm_read_byte(&rainbowColors[4][0]);
                dmaPackage2[25] = (movingLED == 4) ? 0xFF : pgm_read_byte(&rainbowColors[4][1]);
                dmaPackage2[26] = (movingLED == 4) ? 0xFF : pgm_read_byte(&rainbowColors[4][2]);
                
                dmaPackage2[27] = (movingLED == 9) ? 0xFF : pgm_read_byte(&rainbowColors[9][0]);
                dmaPackage2[28] = (movingLED == 9) ? 0xFF : pgm_read_byte(&rainbowColors[9][1]);
                dmaPackage2[29] = (movingLED == 9) ? 0xFF : pgm_read_byte(&rainbowColors[9][2]);
                
                dmaPackage2[30] = (movingLED == 14) ? 0xFF : pgm_read_byte(&rainbowColors[14][0]);
                dmaPackage2[31] = (movingLED == 14) ? 0xFF : pgm_read_byte(&rainbowColors[14][1]);
                dmaPackage2[32] = (movingLED == 14) ? 0xFF : pgm_read_byte(&rainbowColors[14][2]);
                
                dmaPackage2[33] = (movingLED == 19) ? 0xFF : pgm_read_byte(&rainbowColors[19][0]);
                dmaPackage2[34] = (movingLED == 19) ? 0xFF : pgm_read_byte(&rainbowColors[19][1]);
                dmaPackage2[35] = (movingLED == 19) ? 0xFF : pgm_read_byte(&rainbowColors[19][2]);
                
                __enable_irq();
                
                ledState = LED_SEND_PKG1;
            }
            break;

        case LED_SEND_PKG1:
            if (sendPacket(dmaPackage1)) {
                ledState = LED_SEND_PKG2;
                retryCount = 0;
            } else if (++retryCount > 3) {
                ledState = LED_IDLE; // Give up
            }
            break;

        case LED_SEND_PKG2:
            if (sendPacket(dmaPackage2)) {
                ledState = LED_SEND_ACTIVATE;
                retryCount = 0;
            } else if (++retryCount > 3) {
                ledState = LED_IDLE;
            }
            break;

        case LED_SEND_ACTIVATE:
            if (sendPacket(dmaActivation)) {
                ledState = LED_IDLE; // Skip echo wait for now
                commandCount++;
                lastCommandTime = millis();
            } else if (++retryCount > 3) {
                ledState = LED_IDLE;
            }
            break;

        case LED_WAIT_ECHO:
            if (checkEcho()) {
                ledState = LED_IDLE; // Success
            } else if (++retryCount > 10) {
                ledState = LED_IDLE; // Give up
            }
            break;
    }
}

// Non-blocking packet send
bool HIDSelector::sendPacket(uint8_t* packet) {
    uint8_t rcode = pUsb->outTransfer(bAddress, 0x04, 64, packet);
    return (rcode == 0);
}

// Non-blocking echo check
bool HIDSelector::checkEcho() {
    uint8_t buffer[64];
    uint16_t bytesRead = 64;
    uint8_t rcode = pUsb->inTransfer(bAddress, 0x03, &bytesRead, buffer);
    if (rcode == 0 && bytesRead >= 3) {
        return true; // Simple echo check
    }
    return false;
}

USB Usb;
//USBHub Hub(&Usb);
HIDSelector hidSelector(&Usb);

// Global variables at the top
int buttonState = LOW;
const unsigned long debounceDelay = 50;
int timerAnimationCount = 0;

// EP polling statistics
static int epPollCount = 0;
static int epPollErrors = 0;
static int epPollSuccesses = 0;

// Global variables for failure tracking
unsigned long consecutiveNAKFailures = 0;
unsigned long lastSuccessfulCommand = 0;
bool systemRecoveryMode = false;

// Animation cycle counter for status prints
static int animationCycleCount = 0;

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
    UsbDEBUGlvl = 0x00;  // Minimal debug level to eliminate any debug-related timing interference

    delay(200);
}

void loop()
{
    static elapsedMillis loopTimer;
    // USB task needs frequent calls for device stability
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
    
    // ULTRA-MINIMAL: Initialize immediately when device is ready (no delays)
    if (hidSelector.isReady() && !interface1InitAttempted) {
        interface1InitAttempted = true;
        Serial.print("🚀 Device ready at T+");
        Serial.print(loopTimer);
        Serial.println("ms! Initializing Interface 1 for LED control...");
         
        bool success = hidSelector.initializeInterface1();
        if (success) {
            // CRITICAL: Disable automatic polling that causes periodic flickering
            hidSelector.disableAutomaticPolling();
            
            animationStarted = true;
            Serial.println("🌈 Starting ultra-minimal RGB animation for MIDI looper...");
            Serial.println("💡 Animation: Moving white LED on rainbow background");
            Serial.println("🎯 Zero-maintenance mode - optimized for uninterrupted MIDI timing!");
            Serial.print("🕐 Animation started at T+");
            Serial.print(loopTimer);
            Serial.println("ms");
        } else {
            Serial.print("❌ Interface 1 initialization failed at T+");
            Serial.print(loopTimer);
            Serial.println("ms - SINGLE ATTEMPT ONLY (no retries to avoid timing disruption)");
            // NO RETRY - any retry mechanism causes periodic timing disruption
        }
    }
    
    // Run RGB animation synchronized with SOF frames instead of millis()
    static uint16_t animationCount = 0;
    
    if (animationStarted && hidSelector.isReady()) {
        
        // Add settling delay before first animation command
        static bool firstAnimationCommand = true;
        static elapsedMicros settlingTimer;
        static bool settlingStarted = false;
        
        if (firstAnimationCommand) {
            if (!settlingStarted) {
                settlingTimer = 0;
                settlingStarted = true;
                Serial.println("⏳ Device settling before first LED command...");
            }
            // Wait 1000ms (1,000,000μs) before first LED command
            if (settlingTimer < 1000000) {
                return; // Skip animation until settling period is complete
            }
            firstAnimationCommand = false;
            Serial.println("✅ Device settled - starting LED animation");
        }
        
        // BACK TO BASICS: Simple 41ms timing that was giving consistent 12-update cycles
        // MICROSECOND PRECISION: Use elapsedMicros for fine-tuning the optimal cadence
        // This allows testing values like 40500μs, 41200μs, etc. to optimize flickering
        
        static elapsedMicros animationTimer;
        
        // OPTIMAL TIMING: 41ms gives cleanest pattern (consistent 12-update cycles with 0.8s flickering)
        // Frame sync attempts didn't improve this - device has inherent 12-cycle limitation
        // Now we can fine-tune with microsecond precision: 41000μs = 41.000ms
        
        const uint32_t ANIMATION_INTERVAL_MICROS = 100000; // 41.000ms - can be fine-tuned
        
        // SOF-based LED animation (drift-free, 1ms precision)
        static bool sofSetupDone = false;
        if (!sofSetupDone) {
            // Enable SOF interrupts once
            uint8_t hien = Usb.regRd(rHIEN);
            Usb.regWr(rHIEN, hien | bmFRAMEIE);
            Serial.println("🔄 SOF interrupts enabled for drift-free LED timing");
            sofSetupDone = true;
        }
        
        // Check if SOF happened
        uint8_t irq = Usb.regRd(rHIRQ);
        if (irq & bmFRAMEIRQ) {
            // Clear FRAMEIRQ
            Usb.regWr(rHIRQ, bmFRAMEIRQ);
            
            // Handle LED state machine on every SOF (non-blocking)
            hidSelector.handleLEDStateMachine();
        }
    }
    
    // Poll Interface 1 endpoint 0x83 for additional data (frequent polling needed for ACKs!)
    // DISABLED: 100% failure rate shows EP 0x83 doesn't exist on this device
    static elapsedMillis pollTimer;
    static bool pollingEnabled = false; // EP polling completely broken - 100% failure rate
    
    if (pollingEnabled && pollTimer >= 50 && hidSelector.isReady()) { // Poll every 50ms
        pollTimer = 0;
        
        uint8_t buf[64];
        uint16_t len = 64;
        uint8_t rcode = Usb.inTransfer(hidSelector.GetAddress(), 0x83, &len, buf);
        
        epPollCount++;
        if (rcode == 0) {
            epPollSuccesses++;
        } else {
            epPollErrors++;
        }
        
        // Report EP polling statistics every 100 polls (~5 seconds with 50ms intervals)
        if (epPollCount % 100 == 0) {
            Serial.print("📊 EP polling stats: ");
            Serial.print(epPollSuccesses);
            Serial.print(" successes, ");
            Serial.print(epPollErrors);
            Serial.print(" errors, ");
            Serial.print((epPollSuccesses * 100.0) / epPollCount, 1);
            Serial.println("% success rate");
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
    
    // Pattern detection temporarily disabled - it was causing 37-second flickering
    // The 750ms timing interval was interfering with LED commands
    // TODO: Re-implement with non-interfering timing if pattern analysis is still needed
}
