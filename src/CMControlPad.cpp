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
    
    // Register endpoint information with USB library
    uint8_t rcode = pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    if (rcode) {
        Serial.print(F("❌ setEpInfoEntry failed: 0x"));
        Serial.println(rcode, HEX);
        return;
    }
}

void CMControlPad::discoverEndpoints() {
    // Get configuration descriptor to discover actual endpoints
    uint8_t confDescBuf[256];
    uint8_t rcode = pUsb->getConfDescr(bAddress, 0, sizeof(confDescBuf), 1, confDescBuf);
    if (rcode) {
        Serial.print(F("❌ Failed to get config descriptor: 0x"));
        Serial.println(rcode, HEX);
        return;
    }
    
    // Parse configuration descriptor
    uint8_t len = confDescBuf[0];
    if (len < 9) {
        Serial.println(F("❌ Config descriptor too short"));
        return;
    }
    
    uint8_t numInterfaces = confDescBuf[4];
    
    // Parse interface and endpoint descriptors
    uint8_t* ptr = confDescBuf + 9; // Skip config descriptor header
    uint8_t remaining = len - 9;
    
    while (remaining > 0) {
        uint8_t descLen = ptr[0];
        uint8_t descType = ptr[1];
        
        if (descLen > remaining) break;
        
        if (descType == USB_DESCRIPTOR_INTERFACE) {
            if (descLen >= 9) {
                uint8_t ifaceNum = ptr[2];
                uint8_t ifaceClass = ptr[5];
                uint8_t ifaceSubClass = ptr[6];
                uint8_t ifaceProtocol = ptr[7];
                uint8_t numEndpoints = ptr[4];
                
                // Only process Interface 1 (control interface)
                if (ifaceNum == 1) {
                    // Update endpoint addresses based on actual device
                    if (numEndpoints >= 2) {
                        // Look for OUT endpoint (0x04) and IN endpoint (0x83)
                        // This will be done in the next loop through endpoint descriptors
            }
        }
            }
        } else if (descType == USB_DESCRIPTOR_ENDPOINT) {
            if (descLen >= 7) {
                uint8_t epAddr = ptr[2];
                uint8_t epAttribs = ptr[3];
                uint16_t maxPktSize = ptr[4] | (ptr[5] << 8);
                
                // Update endpoint info based on discovered endpoints
                if ((epAddr & 0x80) == 0) { // OUT endpoint
                    if (epAddr == 0x04) {
                        epInfo[epDataOutIndex].epAddr = epAddr;
                        epInfo[epDataOutIndex].maxPktSize = maxPktSize;
                        epInfo[epDataOutIndex].epAttribs = epAttribs;
                    }
                } else { // IN endpoint
                    if (epAddr == 0x83) {
                        epInfo[epDataInIndex].epAddr = epAddr;
                        epInfo[epDataInIndex].maxPktSize = maxPktSize;
                        epInfo[epDataInIndex].epAttribs = epAttribs;
                    }
                }
            }
        }
        
        ptr += descLen;
        remaining -= descLen;
    }
}

uint8_t CMControlPad::initializeDevice() {
    // Activate Interface 0 (HID keyboard)
    uint8_t rcode = activateInterface0();
    if (rcode) {
        Serial.println(F("❌ Interface 0 activation failed"));
        return rcode;
    }
    
    // Activate Interface 1 (control interface)
    rcode = activateInterface1();
    if (rcode) {
        Serial.println(F("❌ Interface 1 activation failed"));
        return rcode;
    }
    
    // Send initialization sequence
    rcode = sendInitializationSequence();
    if (rcode) {
        Serial.println(F("❌ Initialization sequence failed"));
        return rcode;
    }
    
    // Test simple LED command
    rcode = sendSimpleLEDTest();
    if (rcode) {
        // Non-critical - device may not support this command
    }
    
    // Try to activate custom mode
    rcode = activateCustomMode();
    if (rcode) {
        // Non-critical - device may not support this command
    }
    
    return 0;
}

uint8_t CMControlPad::activateInterface0() {
    // HID SET_IDLE request
    uint8_t rcode = pUsb->ctrlReq(bAddress, 0, 0x21, 0x0A, 0x0000, 0, 0, 0, 0, NULL, NULL);
    if (rcode) {
        Serial.print(F("❌ HID SET_IDLE failed: 0x"));
        Serial.println(rcode, HEX);
        return rcode;
    }
    
    return 0;
}

uint8_t CMControlPad::activateInterface1() {
    // For CM Control Pad, Interface 1 should already be active after configuration
    // Skip the problematic SET_INTERFACE call since it causes STALL
    return 0;
}

uint8_t CMControlPad::sendInitializationSequence() {
    // Command 1: 42 00 with flag pattern 01 00 00 01
    uint8_t cmd1[CM_PACKET_SIZE] = {0};
    cmd1[0] = 0x42; cmd1[1] = 0x00; cmd1[2] = 0x00; cmd1[3] = 0x00;
    cmd1[4] = 0x01; cmd1[5] = 0x00; cmd1[6] = 0x00; cmd1[7] = 0x01;
    uint8_t rcode = sendCommandWithProperAck(cmd1);
    if (rcode) return rcode;
    
    // 10ms delay between commands (like USB capture)
    delay(10);
    
    // Command 2: 42 10 with flag pattern 01 00 00 01  
    uint8_t cmd2[CM_PACKET_SIZE] = {0};
    cmd2[0] = 0x42; cmd2[1] = 0x10; cmd2[2] = 0x00; cmd2[3] = 0x00;
    cmd2[4] = 0x01; cmd2[5] = 0x00; cmd2[6] = 0x00; cmd2[7] = 0x01;
    rcode = sendCommandWithProperAck(cmd2);
    if (rcode) return rcode;
    
    delay(10);
    
    // Command 3: 43 00 with flag pattern 01 00 00 00
    uint8_t cmd3[CM_PACKET_SIZE] = {0};
    cmd3[0] = 0x43; cmd3[1] = 0x00; cmd3[2] = 0x00; cmd3[3] = 0x00;
    cmd3[4] = 0x01; cmd3[5] = 0x00; cmd3[6] = 0x00; cmd3[7] = 0x00;
    rcode = sendCommandWithProperAck(cmd3);
    if (rcode) return rcode;
    
    // Longer delay before mode commands
    delay(15);
    epInfo[epDataOutIndex].bmSndToggle = 0;
    pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    delay(5);
    
    // Command 4: 41 80 (status)
    uint8_t cmd4[CM_PACKET_SIZE] = {0};
    cmd4[0] = 0x41; cmd4[1] = 0x80;
    rcode = sendCommandWithProperAck(cmd4);
    if (rcode) return rcode;
    
    delay(10);
    
    // Command 5: 52 00 (activate effects)
    epInfo[epDataOutIndex].bmSndToggle = 0;
    pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    delay(2);
    
    uint8_t cmd5[CM_PACKET_SIZE] = {0};
    cmd5[0] = 0x52; cmd5[1] = 0x00;
    rcode = sendCommandWithProperAck(cmd5);
    if (rcode) return rcode;
    
    delay(10); // Final delay to let device process
    
    return 0;
}

uint8_t CMControlPad::sendCommandWithProperAck(uint8_t* cmd) {
    // Send command
    uint8_t rcode = pUsb->outTransfer(bAddress, epInfo[epDataOutIndex].epAddr, CM_PACKET_SIZE, cmd);
    
    if (rcode) {
        Serial.print(F("❌ Command OUT failed: 0x"));
        Serial.println(rcode, HEX);
        return rcode;
    }
    
    // Wait for acknowledgment - device responds after ~2ms according to USB capture
    delay(3); // Wait 3ms for device to process
    
    uint8_t response[CM_PACKET_SIZE];
    uint16_t len = CM_PACKET_SIZE;
    rcode = pUsb->inTransfer(bAddress, epInfo[epDataInIndex].epAddr, &len, response);
    
    if (rcode == 0 && len > 0) {
        // Verify acknowledgment matches command
        if (response[0] == cmd[0] && response[1] == cmd[1]) {
            return 0; // Success
        } else {
            // Non-critical - acknowledgment doesn't match but continue
            return 0;
        }
    } else if (rcode == 0x7) { // USB_ERROR_TIMEOUT - device might not send immediate ACKs
        return 0; // Continue initialization even without ACK
    } else {
        // Non-critical - continue even without acknowledgment
        return 0;
    }
}

uint8_t CMControlPad::sendSimpleLEDTest() {
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
        return rcode; // Non-critical error
    }
    
    customModeActive = true;
    return 0;
}

uint8_t CMControlPad::activateCustomMode() {
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
        return rcode;
    }
    
    customModeActive = true;
    Serial.println(F("✅ Custom mode activated"));
    return 0;
}

//=============================================================================
// Control Methods
//=============================================================================
bool CMControlPad::setCustomMode() {
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
    return true;
}

bool CMControlPad::setLEDColor(uint8_t button, uint8_t r, uint8_t g, uint8_t b) {
    if (!customModeActive) {
        Serial.println(F("❌ Custom mode not active"));
        return false;
    }
    
    // Implementation for specific LED control would go here
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
    uint8_t rcode = pUsb->outTransfer(bAddress, epInfo[epDataOutIndex].epAddr, len, data);
    
    if (rcode != 0) {
        Serial.print(F("❌ Command OUT failed: 0x"));
        Serial.println(rcode, HEX);
        return rcode;
    }
    
    // Wait for ACK (device responds with ACK packet)
    delay(2); // Give device time to process
    
    uint16_t bytesRead = 64;
    uint8_t buffer[64];
    rcode = pUsb->inTransfer(bAddress, epInfo[epDataInIndex].epAddr, &bytesRead, buffer);
    
    return rcode;
}

uint8_t CMControlPad::manualInit(uint8_t address) {
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
    // Removed verbose output to reduce serial traffic
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