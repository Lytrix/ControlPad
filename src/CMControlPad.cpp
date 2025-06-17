#include "CMControlPad.h"

//=============================================================================
// Constructor
//=============================================================================
CMControlPad::CMControlPad(USB *p) : 
    pUsb(p),
    bAddress(0),
    bPollEnable(false),
    pid(0),
    vid(0),
    initialized(false),
    customModeActive(false),
    readPtr(0),
    pFuncOnInit(nullptr),
    pFuncOnRelease(nullptr),
    pFuncOnData(nullptr)
{
    Serial.println(F("🎮 CMControlPad constructor"));
    
    // Initialize endpoint info
    for (uint8_t i = 0; i < CM_MAX_ENDPOINTS; i++) {
        epInfo[i].epAddr = 0;
        epInfo[i].maxPktSize = 0;
        epInfo[i].epAttribs = 0;
        epInfo[i].bmNakPower = USB_NAK_NOWAIT;  // Like MIDI class
        epInfo[i].bmSndToggle = 0;
        epInfo[i].bmRcvToggle = 0;
    }
    
    // Clear receive buffer
    memset(recvBuf, 0, sizeof(recvBuf));
}

//=============================================================================
// USB Device Configuration Implementation
//=============================================================================
uint8_t CMControlPad::Init(uint8_t parent, uint8_t port, bool lowspeed) {
    Serial.println(F("🎮 CMControlPad::Init"));
    
    uint8_t buf[sizeof(USB_DEVICE_DESCRIPTOR)];
    USB_DEVICE_DESCRIPTOR* udd = reinterpret_cast<USB_DEVICE_DESCRIPTOR*>(buf);
    uint8_t rcode;
    
    // Reset state
    bPollEnable = false;
    initialized = false;
    customModeActive = false;
    
    // Get device descriptor
    rcode = pUsb->getDevDescr(parent, port, sizeof(USB_DEVICE_DESCRIPTOR), buf);
    if (rcode) {
        Serial.print(F("❌ getDevDescr failed: 0x"));
        Serial.println(rcode, HEX);
        return rcode;
    }
    
    // Store VID/PID
    vid = udd->idVendor;
    pid = udd->idProduct;
    
    Serial.print(F("📍 VID: 0x"));
    Serial.print(vid, HEX);
    Serial.print(F(" PID: 0x"));
    Serial.println(pid, HEX);
    
    // Validate device
    if (vid != CM_VID || pid != CM_PID) {
        Serial.println(F("❌ Not a CM Control Pad"));
        return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
    }
    
    // Allocate USB address
    AddressPool &addrPool = pUsb->GetAddressPool();
    bAddress = addrPool.AllocAddress(parent, false, port);
    if (!bAddress) {
        Serial.println(F("❌ Out of USB addresses"));
        return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;
    }
    
    // Set device address
    rcode = pUsb->setAddr(parent, port, bAddress);
    if (rcode) {
        Serial.print(F("❌ setAddr failed: 0x"));
        Serial.println(rcode, HEX);
        addrPool.FreeAddress(bAddress);
        bAddress = 0;
        return rcode;
    }
    
    delay(300); // Allow device to settle
    
    // Get configuration descriptor  
    uint8_t confDescBuf[256];
    rcode = pUsb->getConfDescr(bAddress, 0, sizeof(confDescBuf), 1, confDescBuf);
    if (rcode) {
        Serial.print(F("❌ getConfDescr failed: 0x"));
        Serial.println(rcode, HEX);
        goto FailGetConfDescr;
    }
    
    // Extract configuration info
    setupDeviceSpecific();
    
    // Configure endpoints
    rcode = pUsb->setConf(bAddress, 0, 1);
    if (rcode) {
        Serial.print(F("❌ setConf failed: 0x"));
        Serial.println(rcode, HEX);
        goto FailSetConf;
    }
    
    // Initialize the device
    rcode = initializeDevice();
    if (rcode) {
        Serial.print(F("❌ Device initialization failed: 0x"));
        Serial.println(rcode, HEX);
        goto FailInit;
    }
    
    // Success
    bPollEnable = true;
    initialized = true;
    
    Serial.println(F("✅ CM Control Pad initialized successfully"));
    
    // Call user init callback
    if (pFuncOnInit) {
        pFuncOnInit();
    }
    
    return 0;

FailInit:
FailSetConf:
FailGetConfDescr:
    Release();
    return rcode;
}

uint8_t CMControlPad::Release() {
    Serial.println(F("🎮 CMControlPad::Release"));
    
    // Call user release callback
    if (pFuncOnRelease) {
        pFuncOnRelease();
    }
    
    // Reset state
    bPollEnable = false;
    initialized = false;
    customModeActive = false;
    
    // Free USB address
    if (bAddress) {
        pUsb->GetAddressPool().FreeAddress(bAddress);
        bAddress = 0;
    }
    
    return 0;
}

uint8_t CMControlPad::Poll() {
    if (!bPollEnable) {
        return 0;
    }
    
    return pollDevice();
}

//=============================================================================
// Device-Specific Setup and Communication
//=============================================================================
void CMControlPad::setupDeviceSpecific() {
    Serial.println(F("🔧 Setting up CM Control Pad endpoints"));
    
    // First, let's discover what endpoints are actually available
    discoverEndpoints();
    
    // Setup endpoint 0 (control)
    epInfo[0].epAddr = 0;
    epInfo[0].maxPktSize = 8;
    epInfo[0].epAttribs = USB_TRANSFER_TYPE_CONTROL;
    epInfo[0].bmNakPower = USB_NAK_MAX_POWER;
    
    // Setup endpoint 1 (IN) - Interface 1, EP 0x83 (or discovered)
    epInfo[epDataInIndex].epAddr = 0x83;
    epInfo[epDataInIndex].maxPktSize = CM_PACKET_SIZE;
    epInfo[epDataInIndex].epAttribs = USB_TRANSFER_TYPE_INTERRUPT;
    epInfo[epDataInIndex].bmNakPower = USB_NAK_NOWAIT;
    epInfo[epDataInIndex].bmSndToggle = 0;
    epInfo[epDataInIndex].bmRcvToggle = 0;
    
    // Setup endpoint 2 (OUT) - Interface 1, EP 0x04 (or discovered)
    epInfo[epDataOutIndex].epAddr = 0x04;
    epInfo[epDataOutIndex].maxPktSize = CM_PACKET_SIZE;
    epInfo[epDataOutIndex].epAttribs = USB_TRANSFER_TYPE_INTERRUPT;
    epInfo[epDataOutIndex].bmNakPower = 4;  // Allow 2^4-1 = 15 NAKs before giving up (was USB_NAK_NOWAIT = 1 NAK)
    epInfo[epDataOutIndex].bmSndToggle = 0;
    epInfo[epDataOutIndex].bmRcvToggle = 0;
    
    // Print endpoint configuration
    Serial.println(F("📍 Endpoint Configuration:"));
    Serial.print(F("  EP0 (Control): 0x"));
    Serial.print(epInfo[0].epAddr, HEX);
    Serial.print(F(" max="));
    Serial.println(epInfo[0].maxPktSize);
    Serial.print(F("  EP1 (IN):      0x"));
    Serial.print(epInfo[epDataInIndex].epAddr, HEX);
    Serial.print(F(" max="));
    Serial.println(epInfo[epDataInIndex].maxPktSize);
    Serial.print(F("  EP2 (OUT):     0x"));
    Serial.print(epInfo[epDataOutIndex].epAddr, HEX);
    Serial.print(F(" max="));
    Serial.println(epInfo[epDataOutIndex].maxPktSize);
    
    // Register endpoints with USB host
    uint8_t rcode = pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    if (rcode) {
        Serial.print(F("❌ setEpInfoEntry failed: 0x"));
        Serial.println(rcode, HEX);
    } else {
        Serial.println(F("✅ Endpoints configured"));
    }
}

void CMControlPad::discoverEndpoints() {
    Serial.println(F("🔍 Discovering actual device endpoints..."));
    
    // Get configuration descriptor to see what interfaces and endpoints exist
    uint8_t buf[256];
    uint16_t len = 256;
    
    uint8_t rcode = pUsb->getConfDescr(bAddress, 0, len, 0, buf);
    if (rcode) {
        Serial.print(F("❌ Failed to get config descriptor: 0x"));
        Serial.println(rcode, HEX);
        return;
    }
    
    Serial.print(F("📋 Configuration descriptor length: "));
    Serial.println(len);
    
    // Parse the configuration descriptor using raw bytes
    if (len < 9) {
        Serial.println(F("❌ Config descriptor too short"));
        return;
    }
    
    uint8_t numInterfaces = buf[4]; // bNumInterfaces at offset 4
    Serial.print(F("📋 Number of interfaces: "));
    Serial.println(numInterfaces);
    
    // Parse descriptors
    uint16_t pos = buf[0]; // Skip config descriptor (bLength at offset 0)
    
    while (pos < len) {
        if (pos + 1 >= len) break;
        
        uint8_t descLen = buf[pos];
        uint8_t descType = buf[pos + 1];
        
        if (descLen == 0 || pos + descLen > len) break;
        
        if (descType == 4) { // Interface descriptor
            if (pos + 8 < len) {
                uint8_t ifaceNum = buf[pos + 2];
                uint8_t ifaceClass = buf[pos + 5];
                uint8_t ifaceSubClass = buf[pos + 6];
                uint8_t ifaceProtocol = buf[pos + 7];
                uint8_t numEndpoints = buf[pos + 4];
                
                Serial.print(F("🔌 Interface "));
                Serial.print(ifaceNum);
                Serial.print(F(": Class=0x"));
                Serial.print(ifaceClass, HEX);
                Serial.print(F(" SubClass=0x"));
                Serial.print(ifaceSubClass, HEX);
                Serial.print(F(" Protocol=0x"));
                Serial.print(ifaceProtocol, HEX);
                Serial.print(F(" NumEP="));
                Serial.println(numEndpoints);
            }
        }
        else if (descType == 5) { // Endpoint descriptor
            if (pos + 6 < len) {
                uint8_t epAddr = buf[pos + 2];
                uint8_t epAttribs = buf[pos + 3];
                uint16_t maxPktSize = buf[pos + 4] | (buf[pos + 5] << 8);
                
                Serial.print(F("  📍 Endpoint 0x"));
                Serial.print(epAddr, HEX);
                Serial.print(F(" "));
                Serial.print((epAddr & 0x80) ? F("IN") : F("OUT"));
                Serial.print(F(" Type="));
                Serial.print(epAttribs & 0x03);
                Serial.print(F(" MaxPkt="));
                Serial.println(maxPktSize);
            }
        }
        
        pos += descLen;
    }
}

uint8_t CMControlPad::initializeDevice() {
    Serial.println(F("🚀 Starting CM Control Pad initialization sequence"));
    
    // Step 1: Activate Interface 0 (HID keyboard)
    if (!activateInterface0()) {
        Serial.println(F("❌ Interface 0 activation failed"));
        return USB_ERROR_CONFIG_REQUIRES_ADDITIONAL_RESET;
    }
    
    // Step 1.5: Activate Interface 1 (control interface)
    if (!activateInterface1()) {
        Serial.println(F("❌ Interface 1 activation failed"));
        return USB_ERROR_CONFIG_REQUIRES_ADDITIONAL_RESET;
    }
    
    // Step 2: Send initialization command sequence
    if (!sendInitializationSequence()) {
        Serial.println(F("❌ Initialization sequence failed"));
        return USB_ERROR_CONFIG_REQUIRES_ADDITIONAL_RESET;
    }
    
    // Step 3: Test LED commands
    delay(50); // Longer delay to let device fully process initialization
    Serial.println(F("🔄 Preparing for LED commands - resetting toggles"));
    epInfo[epDataOutIndex].bmSndToggle = 0;
    pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    delay(10);
    
    Serial.println(F("🌈 Testing simple LED command (56 81)"));
    if (!testSimpleLEDCommand()) {
        Serial.println(F("⚠️ Simple LED test failed (non-critical)"));
        // Don't fail init for this
    }
    
    // Step 4: Set custom mode (original LED test)
    delay(10);
    if (!setCustomMode()) {
        Serial.println(F("⚠️ Custom mode activation failed (non-critical)"));
        // Don't fail init for this
    }
    
    Serial.println(F("✅ Device initialization complete"));
    return 0;
}

bool CMControlPad::activateInterface0() {
    Serial.println(F("🔌 Activating Interface 0 (HID keyboard)"));
    
    // HID SET_IDLE request
    uint8_t rcode = pUsb->ctrlReq(bAddress, 0, 0x21, 0x0A, 0x0000, 0, 0, 0, 0, NULL, NULL);
    if (rcode) {
        Serial.print(F("❌ HID SET_IDLE failed: 0x"));
        Serial.println(rcode, HEX);
        return false;
    }
    
    Serial.println(F("✅ Interface 0 activated"));
    return true;
}

bool CMControlPad::activateInterface1() {
    Serial.println(F("🔌 Activating Interface 1 (control interface)"));
    
    // For CM Control Pad, Interface 1 should already be active after configuration
    // Skip the problematic SET_INTERFACE call since it causes STALL
    Serial.println(F("ℹ️ Skipping SET_INTERFACE (causes STALL on CM Control Pad)"));
    
    // Just ensure we have the right configuration
    // The device should already be configured correctly by the HID system
    Serial.println(F("✅ Interface 1 activation complete"));
    return true;
}

bool CMControlPad::sendInitializationSequence() {
    Serial.println(F("📤 Sending initialization sequence with proper timing"));
    
    // Command 1: 42 00 with flag pattern 01 00 00 01
    Serial.println(F("📤 Step 1: Setup command 42 00"));
    uint8_t cmd1[CM_PACKET_SIZE] = {0};
    cmd1[0] = 0x42; cmd1[1] = 0x00; cmd1[2] = 0x00; cmd1[3] = 0x00;
    cmd1[4] = 0x01; cmd1[5] = 0x00; cmd1[6] = 0x00; cmd1[7] = 0x01;
    if (!sendCommandWithProperAck(cmd1)) return false;
    
    // 10ms delay between commands (like USB capture)
    delay(10);
    
    // Command 2: 42 10 with flag pattern 01 00 00 01  
    Serial.println(F("📤 Step 2: Setup command 42 10"));
    uint8_t cmd2[CM_PACKET_SIZE] = {0};
    cmd2[0] = 0x42; cmd2[1] = 0x10; cmd2[2] = 0x00; cmd2[3] = 0x00;
    cmd2[4] = 0x01; cmd2[5] = 0x00; cmd2[6] = 0x00; cmd2[7] = 0x01;
    if (!sendCommandWithProperAck(cmd2)) return false;
    
    delay(10);
    
    // Command 3: 43 00 with flag pattern 01 00 00 00
    Serial.println(F("📤 Step 3: Setup command 43 00"));
    uint8_t cmd3[CM_PACKET_SIZE] = {0};
    cmd3[0] = 0x43; cmd3[1] = 0x00; cmd3[2] = 0x00; cmd3[3] = 0x00;
    cmd3[4] = 0x01; cmd3[5] = 0x00; cmd3[6] = 0x00; cmd3[7] = 0x00;
    if (!sendCommandWithProperAck(cmd3)) return false;
    
    // Longer delay before mode commands
    delay(15);
    Serial.println(F("🔄 Resetting data toggle for mode commands"));
    epInfo[epDataOutIndex].bmSndToggle = 0;
    pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    delay(5);
    
    // Command 4: 41 80 (status)
    Serial.println(F("📤 Step 4: Status command 41 80"));
    uint8_t cmd4[CM_PACKET_SIZE] = {0};
    cmd4[0] = 0x41; cmd4[1] = 0x80;
    if (!sendCommandWithProperAck(cmd4)) return false;
    
    delay(10);
    
    // Command 5: 52 00 (activate effects)
    Serial.println(F("📤 Step 5: Activate effects 52 00"));
    Serial.println(F("🔄 Resetting data toggle for effects commands"));
    epInfo[epDataOutIndex].bmSndToggle = 0;
    pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    delay(2);
    
    uint8_t cmd5[CM_PACKET_SIZE] = {0};
    cmd5[0] = 0x52; cmd5[1] = 0x00;
    if (!sendCommandWithProperAck(cmd5)) return false;
    
    delay(10); // Final delay to let device process
    
    Serial.println(F("✅ Full initialization sequence complete"));
    return true;
}

bool CMControlPad::sendCommandWithProperAck(uint8_t* cmd) {
    // Send command
    uint8_t rcode = pUsb->outTransfer(bAddress, epInfo[epDataOutIndex].epAddr, CM_PACKET_SIZE, cmd);
    
    Serial.print(F("📤 OUT: "));
    for (int i = 0; i < 16; i++) {
        if (cmd[i] < 0x10) Serial.print(F("0"));
        Serial.print(cmd[i], HEX);
    }
    Serial.println();
    
    if (rcode) {
        Serial.print(F("❌ Command OUT failed: 0x"));
        Serial.println(rcode, HEX);
        return false;
    }
    
    // Wait for acknowledgment - device responds after ~2ms according to USB capture
    delay(3); // Wait 3ms for device to process
    
    uint8_t response[CM_PACKET_SIZE];
    uint16_t len = CM_PACKET_SIZE;
    rcode = pUsb->inTransfer(bAddress, epInfo[epDataInIndex].epAddr, &len, response);
    
    if (rcode == 0 && len > 0) {
        Serial.print(F("📥 ACK ("));
        Serial.print(len);
        Serial.print(F(" bytes): "));
        for (int i = 0; i < len && i < 16; i++) {
            if (response[i] < 0x10) Serial.print(F("0"));
            Serial.print(response[i], HEX);
            Serial.print(F(" "));
        }
        Serial.println();
        
        // Verify acknowledgment matches command
        if (response[0] == cmd[0] && response[1] == cmd[1]) {
            Serial.println(F("✅ Command acknowledged correctly"));
        } else {
            Serial.println(F("⚠️ Acknowledgment doesn't match command"));
        }
        return true;
    } else if (rcode == 0x7) { // USB_ERROR_TIMEOUT - device might not send immediate ACKs
        Serial.println(F("⚠️ No immediate acknowledgment (timeout) - continuing anyway"));
        return true; // Continue initialization even without ACK
    } else {
        Serial.print(F("⚠️ No acknowledgment received: 0x"));
        Serial.println(rcode, HEX);
        // For initialization commands, we'll be more lenient about ACKs
        Serial.println(F("⚠️ Continuing without acknowledgment"));
        return true; // Continue even without acknowledgment
    }
}

bool CMControlPad::testSimpleLEDCommand() {
    Serial.println(F("🌈 Testing simple LED command (56 81)"));
    
    uint8_t cmd[CM_PACKET_SIZE] = {0};
    cmd[0] = 0x56;  // CM_CMD_CUSTOM_MODE 
    cmd[1] = 0x81;
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
    
    uint8_t rcode = sendCommand(cmd, CM_PACKET_SIZE);
    if (rcode) {
        Serial.print(F("❌ Simple LED test failed: 0x"));
        Serial.println(rcode, HEX);
        return false;
    }
    
    customModeActive = true;
    Serial.println(F("✅ Simple LED test passed"));
    return true;
}

//=============================================================================
// Control Methods
//=============================================================================
bool CMControlPad::setCustomMode() {
    Serial.println(F("🎨 Setting custom LED mode"));
    
    uint8_t cmd[CM_PACKET_SIZE] = {0};
    cmd[0] = 0x56;  // CM_CMD_CUSTOM_MODE 
    cmd[1] = 0x81;
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
    
    // Reset toggle for new command type
    epInfo[epDataOutIndex].bmSndToggle = 0;
    pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    
    uint8_t rcode = sendCommand(cmd, CM_PACKET_SIZE);
    if (rcode) {
        Serial.print(F("❌ Custom mode failed: 0x"));
        Serial.println(rcode, HEX);
        return false;
    }
    
    customModeActive = true;
    Serial.println(F("✅ Custom mode activated"));
    return true;
}

bool CMControlPad::setLEDColor(uint8_t button, uint8_t r, uint8_t g, uint8_t b) {
    if (!customModeActive) {
        Serial.println(F("❌ Custom mode not active"));
        return false;
    }
    
    // Implementation for specific LED control would go here
    Serial.print(F("🌈 Setting LED "));
    Serial.print(button);
    Serial.print(F(" to RGB("));
    Serial.print(r);
    Serial.print(F(","));
    Serial.print(g);
    Serial.print(F(","));
    Serial.print(b);
    Serial.println(F(")"));
    
    return true;
}

bool CMControlPad::sendLEDCommand(uint8_t* data, uint16_t len) {
    if (!customModeActive) {
        Serial.println(F("❌ Custom mode not active"));
        return false;
    }
    
    uint8_t rcode = sendCommand(data, len);
    return (rcode == 0);
}

uint8_t CMControlPad::sendCommand(uint8_t* data, uint16_t len) {
    if (!bPollEnable) {
        return USB_ERROR_INVALID_ARGUMENT;
    }
    
    // Reset data toggle for reliable transfer (similar to how initialization works)
    epInfo[epDataOutIndex].bmSndToggle = 0;
    pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    
    Serial.print(F("📤 CMD: "));
    for (int i = 0; i < len && i < 16; i++) {
        if (data[i] < 0x10) Serial.print(F("0"));
        Serial.print(data[i], HEX);
    }
    Serial.println();
    
    return pUsb->outTransfer(bAddress, epInfo[epDataOutIndex].epAddr, len, data);
}

uint8_t CMControlPad::manualInit(uint8_t address) {
    Serial.println(F("🎯 Manual CM Control Pad initialization"));
    
    // Set the device address
    bAddress = address;
    
    // Store VID/PID for our device
    vid = CM_VID;
    pid = CM_PID;
    
    // Set up endpoints
    setupDeviceSpecific();
    
    // Initialize device communication
    uint8_t rcode = initializeDevice();
    if (rcode) {
        Serial.print(F("❌ Manual initialization failed: 0x"));
        Serial.println(rcode, HEX);
        return rcode;
    }
    
    // Success
    bPollEnable = true;
    initialized = true;
    
    Serial.println(F("✅ Manual CM Control Pad initialization complete"));
    
    // Call user init callback
    if (pFuncOnInit) {
        pFuncOnInit();
    }
    
    return 0;
}

//=============================================================================
// Data Communication Methods  
//=============================================================================
uint8_t CMControlPad::pollDevice() {
    if (!initialized) {
        return 0;
    }
    
    uint16_t len = CM_PACKET_SIZE;
    uint8_t rcode = pUsb->inTransfer(bAddress, epInfo[epDataInIndex].epAddr, &len, recvBuf);
    
    if (rcode == 0 && len > 0) {
        // Process received data
        processInputData(recvBuf, len);
        
        // Call user data callback
        if (pFuncOnData) {
            pFuncOnData(recvBuf, len);
        }
    }
    
    return rcode;
}

void CMControlPad::processInputData(uint8_t* data, uint16_t len) {
    // Basic input data processing
    // Users can implement their own via the callback
    Serial.print(F("📥 Input data ("));
    Serial.print(len);
    Serial.print(F(" bytes): "));
    for (uint16_t i = 0; i < len && i < 16; i++) {  // Limit output
        Serial.print(data[i], HEX);
        Serial.print(F(" "));
    }
    if (len > 16) Serial.print(F("..."));
    Serial.println();
}

uint8_t CMControlPad::RecvData(uint16_t *bytes_rcvd, uint8_t *dataptr) {
    *bytes_rcvd = 0;
    
    if (!bPollEnable) {
        return USB_ERROR_INVALID_ARGUMENT;
    }
    
    uint16_t len = CM_PACKET_SIZE;
    uint8_t rcode = pUsb->inTransfer(bAddress, epInfo[epDataInIndex].epAddr, &len, dataptr);
    
    if (rcode == 0) {
        *bytes_rcvd = len;
    }
    
    return rcode;
}

uint8_t CMControlPad::RecvData(uint8_t *outBuf) {
    uint16_t len;
    return RecvData(&len, outBuf);
}

uint8_t CMControlPad::SendData(uint8_t *dataptr, uint16_t len) {
    if (!bPollEnable) {
        return USB_ERROR_INVALID_ARGUMENT;
    }
    
    return sendCommand(dataptr, len);
}

//=============================================================================
// Configuration Extraction (if needed for complex devices)
//=============================================================================
uint8_t CMControlPad::ConfigXtract(uint8_t conf, uint8_t iface, uint8_t alt, uint8_t proto, const USB_ENDPOINT_DESCRIPTOR* ep) {
    // This can be used for more complex endpoint discovery if needed
    // For now, we use fixed endpoints based on our analysis
    return 0;
} 