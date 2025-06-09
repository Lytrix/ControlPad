#include <Arduino.h>
#include <USBHost_t36.h>  // For USBHost Task() processing
#include <algorithm>      // For std::swap
#include "ControlPadHardware.h"  // For USBControlPad class
#include "ControlPad.h"
#include "USBSynchronizedPacketController.h"

// ===== USB HOST SETUP (USBHost_t36 standard pattern) =====
// These must be declared at global scope for automatic driver discovery
extern USBHost globalUSBHost;
extern USBHub hub1, hub2;
extern USBHIDParser hid1, hid2, hid3;
extern USBControlPad globalControlPadDriver;

// Create a ControlPad instance
ControlPad controlPad;

// ===== MIDI-TIMED LED CONTROL SYSTEM =====
// Constants for MIDI timing
constexpr int MIDI_PPQN = 192;
constexpr float BPM = 240.0;
constexpr float TICKS_PER_SEC = (BPM / 60.0f) * MIDI_PPQN;  // 384 ticks/sec
constexpr int MICROSECONDS_PER_TICK = 1000000.0 / TICKS_PER_SEC;

// Double-buffered LED data for flicker-free updates
DMAMEM static uint8_t ledBufferA[320] __attribute__((aligned(32)));
DMAMEM static uint8_t ledBufferB[320] __attribute__((aligned(32)));
volatile uint8_t* writeBuffer = ledBufferA;
volatile uint8_t* sendBuffer = ledBufferB;
volatile bool ledReady = false;

// MIDI tick timer
IntervalTimer midiTimer;
volatile bool tickFlag = false;
volatile uint32_t tickCount = 0;

// LED update control
volatile bool ledUpdateRequested = false;
uint8_t currentBeatButton = 0;  // Which button is currently highlighted by the beat
ControlPadColor baseButtonColors[24];  // Base colors for all buttons

// USB Frame timing variables
volatile uint32_t lastUSBFrame = 0;

// Forward declarations
void convertLedColorsToPackets(const ControlPadColor* colors, uint8_t* buffer);
void updateMidiBeatHighlight();
void initializeBaseColors();


// MIDI tick handler - called every tick
void midiTickISR() {
    tickFlag = true;
    tickCount++;
}



// Prepare current LED state into write buffer
void prepareLedFrame() {
    if (ledReady) {
        Serial.println("⚠️ prepareLedFrame: ledReady=true, skipping (previous frame not sent yet)");
        return;  // Previous frame not sent yet
    }
    
    Serial.println("🔍 prepareLedFrame: Starting frame preparation...");
    
    // Use our MIDI system's color array directly (bypass ControlPad color management)
    ControlPadColor currentColors[24];
    for (int i = 0; i < 24; i++) {
        currentColors[i] = baseButtonColors[i];  // Use our direct color array
    }
    
    // Debug: Show first few colors being prepared
    // Serial.printf("🔍 prepareLedFrame: Colors[0]=RGB(%d,%d,%d), Colors[1]=RGB(%d,%d,%d), Colors[%d]=RGB(%d,%d,%d)\n",
    //              currentColors[0].r, currentColors[0].g, currentColors[0].b,
    //              currentColors[1].r, currentColors[1].g, currentColors[1].b,
    //              currentBeatButton, currentColors[currentBeatButton].r, currentColors[currentBeatButton].g, currentColors[currentBeatButton].b);
    
    // Convert to hardware format and fill write buffer - CRITICAL: This was commented out!
    Serial.printf("🔧 Converting colors to packets, writeBuffer=%p\n", writeBuffer);
    convertLedColorsToPackets(currentColors, (uint8_t*)writeBuffer);
    
    // Atomic buffer swap
    __disable_irq();
    std::swap(writeBuffer, sendBuffer);
    ledReady = true;
    __enable_irq();
    
    Serial.printf("✅ prepareLedFrame: Buffer swap complete, ledReady=true, sendBuffer=%p\n", sendBuffer);
}


// Convert LED colors to the ControlPad hardware packet format
void convertLedColorsToPackets(const ControlPadColor* colors, uint8_t* buffer) {
    // Initialize buffer
    memset(buffer, 0, 320); 
    
    // COMMAND 1: Set Custom Mode (0x56 0x81)
    uint8_t* cmd = buffer;
    cmd[0] = 0x56;  // Vendor ID
    cmd[1] = 0x81;  // Set effect command
    cmd[2] = 0x00;  // 0000
    cmd[3] = 0x00;
    cmd[4] = 0x01;  // 01000000
    cmd[5] = 0x00;
    cmd[6] = 0x00;
    cmd[7] = 0x00;
    cmd[8] = 0x02;  // 02000000
    cmd[9] = 0x00;
    cmd[10] = 0x00;
    cmd[11] = 0x00;
    cmd[12] = 0xBB; // bbbbbbbb = custom mode
    cmd[13] = 0xBB;
    cmd[14] = 0xBB;
    cmd[15] = 0xBB;
    // Rest of bytes 16-63 are already zero from memset

    // Debug: Show first few colors being converted
    Serial.printf("🔍 convertLedColorsToPackets: Button1=RGB(%d,%d,%d), Button2=RGB(%d,%d,%d), Button22=RGB(%d,%d,%d), Button23=RGB(%d,%d,%d)\n",
                 colors[0].r, colors[0].g, colors[0].b,
                 colors[1].r, colors[1].g, colors[1].b,
                 colors[21].r, colors[21].g, colors[21].b,
                 colors[22].r, colors[22].g, colors[22].b);
    
    // PACKET 2: Package 1 of 2 (0x56 0x83 0x00)
    uint8_t* package1 = buffer + 64;
    package1[0] = 0x56;  // Vendor ID
    package1[1] = 0x83;  // LED package command
    package1[2] = 0x00;  // Package 1 indicator
    package1[3] = 0x00;
    package1[4] = 0x01;  // Unknown field
    package1[5] = 0x00;
    package1[6] = 0x00;
    package1[7] = 0x00;
    package1[8] = 0x80;  // Unknown field
    package1[9] = 0x01;
    package1[10] = 0x00;
    package1[11] = 0x00;
    package1[12] = 0xFF; // Brightness
    package1[13] = 0x00;
    package1[14] = 0x00;
    package1[15] = 0x00;
    package1[16] = 0x00;
    package1[17] = 0x00;
    package1[18] = 0xFF; // Global brightness
    package1[19] = 0xFF; // Global brightness
    package1[20] = 0x00;
    package1[21] = 0x00;
    package1[22] = 0x00;
    package1[23] = 0x00;
    
    // LED data starts at byte 24 - Column-major layout from test data
    // Column 1: LED indices 0, 5, 10, 15, 20 (buttons 1, 6, 11, 16, 21)
    package1[24] = colors[0].r;  package1[25] = colors[0].g;  package1[26] = colors[0].b;   // Button 1
    package1[27] = colors[5].r;  package1[28] = colors[5].g;  package1[29] = colors[5].b;   // Button 6
    package1[30] = colors[10].r; package1[31] = colors[10].g; package1[32] = colors[10].b;  // Button 11
    package1[33] = colors[15].r; package1[34] = colors[15].g; package1[35] = colors[15].b;  // Button 16
    package1[36] = colors[20].r; package1[37] = colors[20].g; package1[38] = colors[20].b;  // Button 21
    
    // Column 2: LED indices 1, 6, 11, 16, 21 (buttons 2, 7, 12, 17, 22)
    package1[39] = colors[1].r;  package1[40] = colors[1].g;  package1[41] = colors[1].b;   // Button 2
    package1[42] = colors[6].r;  package1[43] = colors[6].g;  package1[44] = colors[6].b;   // Button 7
    package1[45] = colors[11].r; package1[46] = colors[11].g; package1[47] = colors[11].b;  // Button 12
    package1[48] = colors[16].r; package1[49] = colors[16].g; package1[50] = colors[16].b;  // Button 17
    package1[51] = colors[21].r; package1[52] = colors[21].g; package1[53] = colors[21].b;  // Button 22
    
    // Column 3: LED indices 2, 7, 12, 17 (buttons 3, 8, 13, 18) - Button 18 split
    package1[54] = colors[2].r;  package1[55] = colors[2].g;  package1[56] = colors[2].b;   // Button 3
    package1[57] = colors[7].r;  package1[58] = colors[7].g;  package1[59] = colors[7].b;   // Button 8
    package1[60] = colors[12].r; package1[61] = colors[12].g; package1[62] = colors[12].b;  // Button 13
    package1[63] = colors[17].r; // Button 18 (partial - only red channel fits)
    
    // PACKET 3: Package 2 of 2 (0x56 0x83 0x01)
    uint8_t* package2 = buffer + 128;
    package2[0] = 0x56;  // Vendor ID
    package2[1] = 0x83;  // LED package command
    package2[2] = 0x01;  // Package 2 indicator
    package2[3] = 0x00;
    
    // Complete Button 18 (green, blue channels from Package 1)
    package2[4] = colors[17].g;  // Button 18 green
    package2[5] = colors[17].b;  // Button 18 blue
    
    // Button 23
    package2[6] = colors[22].r;  package2[7] = colors[22].g;  package2[8] = colors[22].b;   // Button 23
    
    // Column 4: LED indices 3, 8, 13, 18, 23 (buttons 4, 9, 14, 19, 24)
    package2[9] = colors[3].r;   package2[10] = colors[3].g;  package2[11] = colors[3].b;   // Button 4
    package2[12] = colors[8].r;  package2[13] = colors[8].g;  package2[14] = colors[8].b;   // Button 9
    package2[15] = colors[13].r; package2[16] = colors[13].g; package2[17] = colors[13].b;  // Button 14
    package2[18] = colors[18].r; package2[19] = colors[18].g; package2[20] = colors[18].b;  // Button 19
    package2[21] = colors[23].r; package2[22] = colors[23].g; package2[23] = colors[23].b;  // Button 24
    
    // Column 5: LED indices 4, 9, 14, 19 (buttons 5, 10, 15, 20)
    package2[24] = colors[4].r;  package2[25] = colors[4].g;  package2[26] = colors[4].b;   // Button 5
    package2[27] = colors[9].r;  package2[28] = colors[9].g;  package2[29] = colors[9].b;   // Button 10
    package2[30] = colors[14].r; package2[31] = colors[14].g; package2[32] = colors[14].b;  // Button 15
    package2[33] = colors[19].r; package2[34] = colors[19].g; package2[35] = colors[19].b;  // Button 20
    
    // PACKET 4: Apply command (0x41 0x80)
    uint8_t* apply1 = buffer + 192;
    apply1[0] = 0x41;
    apply1[1] = 0x80;
    
    // PACKET 5: Final apply with brightness (0x51 0x28)
    uint8_t* apply2 = buffer + 256;
    apply2[0] = 0x51;
    apply2[1] = 0x28;
    apply2[4] = 0xFF; // Brightness
    
    // Debug: Show packet structure
    Serial.printf("🔧 Packet1[24-29]: %02X%02X%02X %02X%02X%02X (Button1, Button6)\n",
                 package1[24], package1[25], package1[26], package1[27], package1[28], package1[29]);
    Serial.printf("🔧 Packet1[39-44]: %02X%02X%02X %02X%02X%02X (Button2, Button7)\n",
                 package1[39], package1[40], package1[41], package1[42], package1[43], package1[44]);
    Serial.printf("🔧 Packet2[4-8]: %02X%02X %02X%02X%02X (Button18-GB, Button23)\n",
                 package2[4], package2[5], package2[6], package2[7], package2[8]);
}

// Direct USB send that bypasses all monitoring and queue logic
bool sendDirectUSB(const uint8_t* data, size_t length) {
    // Use the new direct USB method that bypasses monitoring
    extern USBControlPad globalControlPadDriver;
    return globalControlPadDriver.sendDirectUSB(data, length);
}

// Send LED packets using proper USBHost_t36 queue system
void sendLedPacketPhase() {
    if (!ledReady) {
        Serial.println("⚠️ sendLedPacketPhase: ledReady=false, nothing to send");
        return;
    }
    
    if (!controlPad.isConnected()) {
        Serial.println("⚠️ sendLedPacketPhase: ControlPad not connected");
        return;
    }
    
    Serial.printf("🚀 sendLedPacketPhase: Starting packet send, sendBuffer=%p\n", sendBuffer);
    
    // Use mixed approach: Direct USB for LED data, queue for Apply commands
    extern USBControlPad globalControlPadDriver;
    
    // Extract the 4 packets from sendBuffer (256 bytes total)
    uint8_t* cmd = (uint8_t*)sendBuffer + 0;;
    uint8_t* package1 = (uint8_t*)sendBuffer + 64;     // Package 1: bytes 0-63
    uint8_t* package2 = (uint8_t*)sendBuffer + 128;    // Package 2: bytes 64-127
    uint8_t* apply1 = (uint8_t*)sendBuffer + 192;     // Apply 1: bytes 128-191
    uint8_t* apply2 = (uint8_t*)sendBuffer + 256;     // Apply 2: bytes 192-255
    
    // Debug: Show packet headers
    Serial.printf("🔧 Packet headers: CMD[0-2]=%02X%02X%02X, P1[0-2]=%02X%02X%02X, P2[0-2]=%02X%02X%02X, A1[0-1]=%02X%02X, A2[0-1]=%02X%02X\n",
                 cmd[0], cmd[1], cmd[2],
                 package1[0], package1[1], package1[2],
                 package2[0], package2[1], package2[2],
                 apply1[0], apply1[1],
                 apply2[0], apply2[1]);
     // 100us delay
    // HACK: Send LED data packets back-to-back using direct USB to avoid queue delays
    Serial.println("⚡ Sending LED data packets back-to-back (direct USB)...");
    bool result0 = globalControlPadDriver.sendCommand(cmd, 64);    // Apply 1 - QUEUED
    bool result1 = globalControlPadDriver.sendCommand(package1, 64);  // Package 1 - DIRECT
    delayMicroseconds(2000);
    bool result2 = globalControlPadDriver.sendCommand(package2, 64);  // Package 2 - DIRECT
    // Brief micro-delay to let LED data settle, then send Apply commands via queue
    delayMicroseconds(2000);
    // Critical: Apply commands through queue with retries
    bool result3 = globalControlPadDriver.sendCommand(apply1, 64);    // Apply 1 - QUEUED
    if (!result3) {
        Serial.println("⚠️ Apply1 failed, retrying...");
        delay(5);  // Brief delay
        result3 = globalControlPadDriver.sendCommand(apply1, 64);
    }
    bool result4 = globalControlPadDriver.sendCommand(apply2, 64);    // Apply 2 - QUEUED
    if (!result4) {
        Serial.println("⚠️ Apply2 failed, retrying...");
        delay(5);  // Brief delay
        result4 = globalControlPadDriver.sendCommand(apply2, 64);
    }
    
    Serial.printf("📤 Mixed send: C0(direct)=%s, P1(direct)=%s, P2(direct)=%s, A1(queue)=%s, A2(queue)=%s\n", 
                 result0 ? "OK" : "FAIL",
                 result1 ? "OK" : "FAIL",
                 result2 ? "OK" : "FAIL",
                 result3 ? "OK" : "FAIL",
                 result4 ? "OK" : "FAIL");
    
    if (!result3 || !result4) {
        Serial.println("❌ CRITICAL: Apply commands failed - LEDs will not update!");
    }
    
    ledReady = false;  // Frame complete, prepare next one
    Serial.println("✅ sendLedPacketPhase: Complete, ledReady=false");
}

// Simple LED update function - let USBHost_t36 handle timing naturally
void sendLedUpdate() {
    Serial.println("🎯 sendLedUpdate: Called - preparing and sending frame");
    // Trust USBHost_t36 to handle packet timing naturally
    prepareLedFrame();
    sendLedPacketPhase();
    Serial.println("🎯 sendLedUpdate: Complete");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🚀 ControlPad Starting with MIDI-Timed LED Control...");
    
    // ===== USB HOST INITIALIZATION (USBHost_t36 standard pattern) =====
    globalUSBHost.begin();
    Serial.println("🔌 USB Host started");
    
    // Give USB Host time to initialize
    for (int i = 0; i < 30; i++) {
        globalUSBHost.Task();
        delay(100);
    }
    
    delay(500);
    
    if (!controlPad.begin()) {
        Serial.println("❌ Failed to initialize ControlPad");
        return;
    }
    
    Serial.println("✅ ControlPad initialized");
    
    delay(2000);  // Give device time to complete activation sequence
    
    // Disable the old LED update system
    controlPad.enableSmartUpdates(false);
    controlPad.enableInstantUpdates(false);
    
    // Initialize base colors for MIDI beat visualization
    initializeBaseColors();
    controlPad.setAllButtonColors(baseButtonColors);
    
    // Start button animation
    controlPad.enableAnimation();
    
    // Initialize the USB-synchronized controller
    usbSyncController.initialize();
    
    // Start MIDI timer for precise LED timing
    midiTimer.begin(midiTickISR, MICROSECONDS_PER_TICK);
    
    // Force an initial LED update to show initial colors
    Serial.printf("🔌 Device connection status: %s\n", controlPad.isConnected() ? "CONNECTED" : "NOT CONNECTED");
    Serial.println("🌈 Forcing initial LED update to display base colors...");
    sendLedUpdate();
    
    Serial.printf("🎵 MIDI timer started: %d us/tick (%.1f Hz)\n", MICROSECONDS_PER_TICK, TICKS_PER_SEC);
    Serial.println("🥁 Ready - MIDI beat visualization active, press buttons for highlighting");
    Serial.println("📋 LED update timing:");
    Serial.println("   🎵 MIDI tick: every 2604us (384 Hz) - for beat visualization only");  
    Serial.println("   📦 LED updates: immediate on beat changes and button presses");
    Serial.println("   💡 USB timing: natural USBHost_t36 packet scheduling");
    Serial.println("   🥁 Beat highlight: every 192 ticks (500ms, 2 Hz @ 120 BPM)");
}

void loop() {
    static uint32_t loopCounter = 0;
    static unsigned long lastDebug = 0;
    
    loopCounter++;
    
    // *** CRITICAL: USB Host Task() - must be called frequently ***
    if (millis() % 5000 == 0) {
        globalUSBHost.Task();
    }
    // *** MIDI-TIMED LED PROCESSING ***
    if (tickFlag) {
        tickFlag = false;
        
        // MIDI processing would go here for looper application
        // ...
        
        // Update MIDI beat highlight every 192 ticks (1 beat @ 120 BPM, 192 PPQN)
        if (tickCount % 192 == 0) {
            updateMidiBeatHighlight();
            // Simple LED update after beat change
            sendLedUpdate();
        }
    }
    
    // *** BUTTON EVENT PROCESSING ***
    // ControlPadEvent event;
    // while (controlPad.pollEvent(event)) {
    //     if (event.type == ControlPadEventType::Button) {
    //         // Update button highlighting immediately
    //         controlPad.setButtonHighlight(event.button.button, event.button.pressed);
            
    //         // Simple LED update for responsive feedback
    //         sendLedUpdate();
            
    //         Serial.printf("🎮 Button %d %s\n", 
    //                      event.button.button + 1,
    //                      event.button.pressed ? "PRESSED" : "RELEASED");
    //     }
    // }
    
    // Minimal status output every 10 seconds
    if (millis() - lastDebug >= 10000) {
        Serial.printf("⚡ Main Loop: %lu cycles, MIDI ticks: %lu\n", 
                     loopCounter, tickCount);
        lastDebug = millis();
    }
}

// Initialize base colors for all buttons (subtle blue)
void initializeBaseColors() {
    Serial.println("🎨 Initializing base colors for all 24 buttons...");
    for (int i = 0; i < 24; i++) {
        if (i == 0) {
            baseButtonColors[i] = {255, 0, 0};  // Bright red for button 1 (testing)
        } else {
            baseButtonColors[i] = {40, 80, 160};  // More visible blue for other buttons
        }
    }
    Serial.printf("🎨 Base colors set: RGB(%d,%d,%d) for all buttons\n", 
                 baseButtonColors[0].r, baseButtonColors[0].g, baseButtonColors[0].b);
}

// Update MIDI beat highlight - cycles through buttons at 120 BPM
void updateMidiBeatHighlight() {
    if (!controlPad.isAnimationEnabled()) return;
    
    // Reset all buttons to base blue color
    for (int i = 0; i < 24; i++) {
        if (i == 0) {
            baseButtonColors[i] = {255, 0, 0};  // Keep button 1 red for testing
        } else {
            baseButtonColors[i] = {40, 80, 160};  // Blue for other buttons
        }
    }
    
    // Highlight the current beat button with bright white
    baseButtonColors[currentBeatButton] = {255, 255, 255};  // Bright white highlight
    
    Serial.printf("🥁 MIDI Beat %lu: Highlighting button %d (RGB: %d,%d,%d), next will be %d\n", 
                 tickCount / 192, currentBeatButton + 1,
                 baseButtonColors[currentBeatButton].r, baseButtonColors[currentBeatButton].g, baseButtonColors[currentBeatButton].b,
                 (currentBeatButton + 1) % 24 + 1);
    
    // Advance to next button for next beat
    currentBeatButton = (currentBeatButton + 1) % 24;
}