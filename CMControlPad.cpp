#include "CMControlPad.h"

uint8_t CMControlPad::Init(uint8_t parent, uint8_t port, bool lowspeed) {
    uint8_t buf[sizeof(USB_DEVICE_DESCRIPTOR)];
    USB_DEVICE_DESCRIPTOR* udd = reinterpret_cast<USB_DEVICE_DESCRIPTOR*>(buf);
    uint8_t rcode;
    uint8_t num_of_conf;
    
    // Get device descriptor
    rcode = pUsb->getDevDescr(parent, port, sizeof(USB_DEVICE_DESCRIPTOR), buf);
    if (rcode) {
        Serial.print("❌ getDevDescr failed with code: 0x");
        Serial.println(rcode, HEX);
        return rcode;
    }
    
    // Check VID/PID
    if (udd->idVendor != CM_VID || udd->idProduct != CM_PID) {
        Serial.println("❌ Device is not a CMControlPad");
        return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
    }
    
    // Get number of configurations
    num_of_conf = udd->bNumConfigurations;
    Serial.print("📋 Number of configurations: ");
    Serial.println(num_of_conf);
    
    // Get configuration descriptor
    USB_CONFIGURATION_DESCRIPTOR confDesc;
    rcode = pUsb->getConfDescr(parent, port, sizeof(USB_CONFIGURATION_DESCRIPTOR), 0, (uint8_t*)&confDesc);
    if (rcode) {
        Serial.print("❌ getConfDescr failed with code: 0x");
        Serial.println(rcode, HEX);
        return rcode;
    }

    // Get full configuration descriptor
    uint8_t buf2[512];
    rcode = pUsb->getConfDescr(parent, port, confDesc.wTotalLength, 0, buf2);
    if (rcode) {
        Serial.print("❌ getConfDescr (full) failed with code: 0x");
        Serial.println(rcode, HEX);
        return rcode;
    }

    Serial.println("\n📋 Full Configuration Descriptor Analysis:");
    Serial.print("Total Length: ");
    Serial.println(confDesc.wTotalLength);
    Serial.print("Number of Interfaces: ");
    Serial.println(confDesc.bNumInterfaces);
    
    // Print raw descriptor data for debugging
    Serial.println("\n🔍 Raw Descriptor Data:");
    for (uint16_t i = 0; i < confDesc.wTotalLength; i++) {
        if (i % 16 == 0) {
            Serial.println();
            Serial.print(i, HEX);
            Serial.print(": ");
        }
        Serial.print(buf2[i], HEX);
        Serial.print(" ");
    }
    Serial.println("\n");
    
    // Step 4: Parse descriptors (fix vendor interface detection)
    uint8_t descBuf[256];
    rcode = pUsb->getConfDescr(parent, port, sizeof(descBuf), 1, descBuf);
    if (rcode) {
        Serial.print(F("❌ Failed to get configuration descriptor: "));
        Serial.println(rcode, HEX);
        return rcode;
    }

    uint8_t fullConfDescrLen = ((USB_CONFIGURATION_DESCRIPTOR*)descBuf)->wTotalLength;
    uint8_t* fullDescBuf = (uint8_t*)malloc(fullConfDescrLen);
    rcode = pUsb->getConfDescr(parent, port, fullConfDescrLen, 1, fullDescBuf);
    if (rcode) {
        Serial.print(F("❌ Failed to get full configuration descriptor: "));
        Serial.println(rcode, HEX);
        free(fullDescBuf);
        return rcode;
    }

    Serial.print(F("📋 Configuration descriptor length: "));
    Serial.println(fullConfDescrLen);

    USB_CONFIGURATION_DESCRIPTOR fullConfDesc = *((USB_CONFIGURATION_DESCRIPTOR*)fullDescBuf);
    Serial.print(F("📋 bNumInterfaces: "));
    Serial.print(fullConfDesc.bNumInterfaces);
    Serial.print(F(", wTotalLength: "));
    Serial.println(fullConfDesc.wTotalLength);

    Serial.println("\n");

    // Parse descriptors to find endpoints
    uint8_t currentInterface = 255;
    uint8_t interface1InEndpoint = 0;
    uint8_t interface1OutEndpoint = 0;
    uint8_t controlInterface = 255;
    uint8_t controlInEndpoint = 0;
    uint8_t controlOutEndpoint = 0;

    for (uint16_t i = 0; i < fullConfDesc.wTotalLength; ) {
        uint8_t* p = fullDescBuf + i;
        
        if (p[1] == USB_DESCRIPTOR_INTERFACE) {
            currentInterface = p[2];
            Serial.print(F("🔍 Found interface: "));
            Serial.print(currentInterface);
            Serial.print(F(" Class: 0x"));
            Serial.print(p[5], HEX);
            Serial.print(F(" SubClass: 0x"));
            Serial.print(p[6], HEX);
            Serial.print(F(" Protocol: 0x"));
            Serial.println(p[7], HEX);
            
            // Look for control interface (class 255)
            if (p[5] == 255) {
                controlInterface = currentInterface;
                Serial.print(F("📍 Control interface: "));
                Serial.println(controlInterface);
            }
        }
        else if (p[1] == USB_DESCRIPTOR_ENDPOINT && currentInterface == 1) {
            // Detect endpoints for interface 1
            if (p[2] & 0x80) {
                interface1InEndpoint = p[2];
                Serial.print(F("📥 Interface 1 IN: 0x"));
                Serial.println(interface1InEndpoint, HEX);
            } else {
                interface1OutEndpoint = p[2];
                Serial.print(F("📤 Interface 1 OUT: 0x"));
                Serial.println(interface1OutEndpoint, HEX);
            }
        }
        else if (p[1] == USB_DESCRIPTOR_ENDPOINT && currentInterface == controlInterface) {
            // Also detect control interface endpoints for reference
            if (p[2] & 0x80) {
                controlInEndpoint = p[2];
                Serial.print(F("📥 Control IN: 0x"));
                Serial.println(controlInEndpoint, HEX);
            } else {
                controlOutEndpoint = p[2];
                Serial.print(F("📤 Control OUT: 0x"));
                Serial.println(controlOutEndpoint, HEX);
            }
        }
        
        i += p[0];
    }

    // Check if we found interface 1 endpoints
    if (interface1InEndpoint == 0 || interface1OutEndpoint == 0) {
        Serial.println(F("❌ Interface 1 endpoints not found"));
        free(fullDescBuf);
        return USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE;
    }

    // Use interface 1 endpoints for commands
    controlInEndpoint = interface1InEndpoint;
    controlOutEndpoint = interface1OutEndpoint;
    Serial.print(F("📍 Using Interface 1 endpoints: IN=0x"));
    Serial.print(controlInEndpoint, HEX);
    Serial.print(F(" OUT=0x"));
    Serial.println(controlOutEndpoint, HEX);
    
    // Set up endpoint information array like AMBX example
    epInfo[0].epAddr = 0;
    epInfo[0].maxPktSize = 8;
    epInfo[0].epAttribs = 0;
    epInfo[0].bmNakPower = USB_NAK_MAX_POWER;
    epInfo[0].bmSndToggle = 0;
    epInfo[0].bmRcvToggle = 0;
    
    // IN endpoint
    epInfo[1].epAddr = controlInEndpoint;
    epInfo[1].maxPktSize = 64;
    epInfo[1].epAttribs = USB_TRANSFER_TYPE_INTERRUPT;
    epInfo[1].bmNakPower = USB_NAK_NOWAIT;
    epInfo[1].bmSndToggle = 0;
    epInfo[1].bmRcvToggle = 0;
    
    // OUT endpoint  
    epInfo[2].epAddr = controlOutEndpoint;
    epInfo[2].maxPktSize = 64;
    epInfo[2].epAttribs = USB_TRANSFER_TYPE_INTERRUPT;
    epInfo[2].bmNakPower = USB_NAK_NOWAIT;
    epInfo[2].bmSndToggle = 0;
    epInfo[2].bmRcvToggle = 0;
    
    Serial.println(F("📍 Pipe configuration:"));
    Serial.print(F("  Control: addr=0 maxPkt=8"));
    Serial.println();
    Serial.print(F("  IN: addr=0x"));
    Serial.print(controlInEndpoint, HEX);
    Serial.print(F(" maxPkt=64"));
    Serial.println();
    Serial.print(F("  OUT: addr=0x"));
    Serial.print(controlOutEndpoint, HEX);
    Serial.print(F(" maxPkt=64"));
    Serial.println();
    
    // Allocate USB address using proper USB Host Shield Library method
    bAddress = pUsb->GetAddressPool().AllocAddress(parent, false, port);
    if (!bAddress) {
        Serial.println(F("❌ Unable to allocate USB address"));
        free(fullDescBuf);
        return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
    }
    
    Serial.print(F("📍 Allocated USB address: "));
    Serial.println(bAddress);
    
    // Set endpoint information
    rcode = pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    if (rcode) {
        Serial.print(F("❌ setEpInfoEntry failed: "));
        Serial.println(rcode, HEX);
        pUsb->GetAddressPool().FreeAddress(bAddress);
        bAddress = 0;
        free(fullDescBuf);
        return rcode;
    }
    Serial.println(F("✅ Endpoints configured - IN: 0x"));
    Serial.print(controlInEndpoint, HEX);
    Serial.print(F(" OUT: 0x"));
    Serial.println(controlOutEndpoint, HEX);
    
    // Set address and configuration using proper USB Host Shield Library sequence
    rcode = pUsb->setAddr(parent, port, bAddress);
    if (rcode) {
        Serial.print(F("❌ Set address failed: "));
        Serial.println(rcode, HEX);
        pUsb->GetAddressPool().FreeAddress(bAddress);
        bAddress = 0;
        free(fullDescBuf);
        return rcode;
    }
    Serial.println(F("✅ Address set"));
    
    rcode = pUsb->setConf(bAddress, epInfo[0].epAddr, 1);
    if (rcode) {
        Serial.print(F("❌ Set configuration failed: "));
        Serial.println(rcode, HEX);
        free(fullDescBuf);
        return rcode;
    }
    Serial.println(F("✅ Configuration set"));
    free(fullDescBuf);
    delay(300);
    
    // Send activation sequence using proper pipe management
    Serial.println(F("🔧 Starting activation sequence..."));
    bool success = true;

    // Multi-packet sequence: Send both 0x42 commands without reading responses
    Serial.println(F("📤 Multi-packet: 0x42 sequence"));
    
    // Step 1: 0x42 00 activation (part 1 of multi-packet)
    Serial.println(F("📤 Step 1: 0x42 00 activation"));
    uint8_t cmd1[64] = {0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd1);
    if (rcode) {
        Serial.print(F("❌ Step 1 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 1 sent"));
    }
    delay(10);

    // Step 2: 0x42 10 variant (part 2 of multi-packet)
    Serial.println(F("📤 Step 2: 0x42 10 variant"));
    uint8_t cmd2[64] = {0x42, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd2);
    if (rcode) {
        Serial.print(F("❌ Step 2 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 2 sent"));
    }
    delay(200); // Longer pause after multi-packet sequence

    // Step 3: 0x43 00 button activation
    Serial.println(F("📤 Step 3: 0x43 00 button activation"));
    uint8_t cmd3[64] = {0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd3);
    if (rcode) {
        Serial.print(F("❌ Step 3 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 3 sent"));
    }
    delay(100);

    // Step 4: 0x41 80 status
    Serial.println(F("📤 Step 4: 0x41 80 status"));
    uint8_t cmd4[64] = {0x41, 0x80, 0x00, 0x00};
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd4);
    if (rcode) {
        Serial.print(F("❌ Step 4 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 4 sent"));
    }
    delay(100);

    // Step 5: 0x52 00 activate effect modes
    Serial.println(F("📤 Step 5: 0x52 00 activate effect modes"));
    uint8_t cmd5[64] = {0x52, 0x00, 0x00, 0x00};
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd5);
    if (rcode) {
        Serial.print(F("❌ Step 5 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 5 sent"));
    }
    delay(100);

    // Step 6: 0x41 80 status (final)
    Serial.println(F("📤 Step 6: 0x41 80 status (final)"));
    uint8_t cmd6[64] = {0x41, 0x80, 0x00, 0x00};
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd6);
    if (rcode) {
        Serial.print(F("❌ Step 6 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 6 sent"));
    }

    if (success) {
        Serial.println(F("✅ Activation sequence completed successfully"));
    } else {
        Serial.println(F("⚠️ Activation sequence had failures"));
    }

    Serial.println(F("✅ Init complete"));
    return 0;
}

uint8_t CMControlPad::Release() {
    Serial.println("🎮 CMControlPad::Release called");
    if (bAddress) {
    pUsb->GetAddressPool().FreeAddress(bAddress);
    bAddress = 0;
    }
    return 0;
}

uint8_t CMControlPad::ConfigXtract(uint8_t conf, uint8_t iface, uint8_t alt, uint8_t proto, const USB_ENDPOINT_DESCRIPTOR* ep) {
    Serial.println("\n📋 Configuration Descriptor Analysis:");
    Serial.print("  Configuration: ");
    Serial.println(conf);
    Serial.print("  Interface: ");
    Serial.println(iface);
    Serial.print("  Alt Setting: ");
    Serial.println(alt);
    Serial.print("  Protocol: ");
    Serial.println(proto);
    
    if (ep) {
        Serial.print("  Endpoint Address: 0x");
        Serial.println(ep->bEndpointAddress, HEX);
        Serial.print("  Endpoint Attributes: 0x");
        Serial.println(ep->bmAttributes, HEX);
        Serial.print("  Max Packet Size: ");
        Serial.println(ep->wMaxPacketSize);
        Serial.print("  Polling Interval: ");
        Serial.println(ep->bInterval);
    }
    
    // Just log interface information - endpoint setup is handled in Init()
    if (iface == 1) {
        Serial.println("\n🎯 Found interface 1 - this contains our target endpoints");
        if (ep) {
            Serial.print("🔍 Endpoint: Addr=0x");
            Serial.print(ep->bEndpointAddress, HEX);
            Serial.print(" Attr=0x");
            Serial.print(ep->bmAttributes, HEX);
            Serial.print(" MaxPkt=");
            Serial.println(ep->wMaxPacketSize);
        }
    }
    
    if (iface == 3) {
        Serial.println("\n🎯 Found interface 3 - vendor/control interface (ignored)");
        if (ep) {
            Serial.print("🔍 Endpoint: Addr=0x");
            Serial.print(ep->bEndpointAddress, HEX);
            Serial.print(" Attr=0x");
            Serial.print(ep->bmAttributes, HEX);
            Serial.print(" MaxPkt=");
            Serial.println(ep->wMaxPacketSize);
        }
    }
    
    return 0;
}

void CMControlPad::sendControlData(uint8_t *data, uint16_t len) {
    Serial.print("📤 Sending command: ");
    for (uint8_t i = 0; i < len && i < 8; i++) {  // Only show first 8 bytes to avoid cluttering
        Serial.print(data[i], HEX);
        Serial.print(" ");
    }
    Serial.println();
    
    pUsb->outTransfer(bAddress, epInfo[2].epAddr, len, data);
}

void CMControlPad::sendCommitCommand() {
    uint8_t commit[64] = {0};
    commit[0] = 0x44; // adjust if needed
    Serial.println("📤 Sending commit command (0x44)");
    sendControlData(commit, 64);
}

bool CMControlPad::initializeProfiles() {
    uint8_t profileInit[64] = {0};
    profileInit[0] = 0x55;
    Serial.println("📤 Sending profile init command (0x55)");
    sendControlData(profileInit, 64);
    return true;
}

bool CMControlPad::switchToCustomMode() {
    uint8_t customMode[64] = {0};
    customMode[0] = 0x60;
    Serial.println("📤 Sending custom mode command (0x60)");
    sendControlData(customMode, 64);
    return true;
}

uint8_t CMControlPad::initializeDevice() {
    static bool initialized = false;
    static uint8_t initAttempts = 0;
    
    if (initialized) {
        initAttempts++;
        if (initAttempts <= 3) {
            Serial.println(F("Device already initialized"));
        } else if (initAttempts == 4) {
            Serial.println(F("Device already initialized (suppressing further messages)"));
        }
        return 0;
    }
    
    Serial.println(F("🔧 Initializing device..."));

    // Step 1: Initialize USB Host Shield
    Serial.println(F("🔧 Initializing USB Host Shield..."));
    
    // Temporarily disable Serial output to suppress USB Host Shield messages
    Serial.flush();
    Serial.end();
    delay(100);
    
    uint8_t rcode = pUsb->Init();
    
    // Re-enable Serial output
    Serial.begin(115200);
    delay(100);
    
    if (rcode) {
        Serial.print(F("❌ USB Host Shield initialization failed: "));
        Serial.println(rcode, HEX);
        return rcode;
    }
    Serial.println(F("✅ USB Host Shield initialized"));
    delay(200);

    // Step 2: Get device descriptor with retry logic
    uint8_t devDesc[18];
    uint8_t retries = 3;
    
    do {
        delay(100); // Wait before attempting
        rcode = pUsb->getDevDescr(bAddress, 0, sizeof(devDesc), devDesc);
        if (rcode == 0) break;
        
        Serial.print(F("⚠️ Device descriptor attempt failed: 0x"));
        Serial.println(rcode, HEX);
        retries--;
    } while (retries > 0);
    
    if (rcode) {
        Serial.print(F("❌ Device descriptor failed after retries: "));
        Serial.println(rcode, HEX);
        return rcode;
    }
    Serial.println(F("✅ Device descriptor OK"));
    delay(200);

    // Step 3: Set configuration
    rcode = pUsb->setConf(bAddress, 0, 1);
    if (rcode) {
        Serial.print(F("❌ Config failed: "));
        Serial.println(rcode, HEX);
        return rcode;
    }
    Serial.println(F("✅ Configuration set"));
    delay(300);

    // Step 4: Parse descriptors (fix vendor interface detection)
    uint8_t descBuf[256];
    rcode = pUsb->getConfDescr(bAddress, 0, sizeof(descBuf), 1, descBuf);
    if (rcode) {
        Serial.print(F("❌ Failed to get configuration descriptor: "));
        Serial.println(rcode, HEX);
        return rcode;
    }

    uint8_t fullConfDescrLen = ((USB_CONFIGURATION_DESCRIPTOR*)descBuf)->wTotalLength;
    uint8_t* fullDescBuf = (uint8_t*)malloc(fullConfDescrLen);
    rcode = pUsb->getConfDescr(bAddress, 0, fullConfDescrLen, 1, fullDescBuf);
    if (rcode) {
        Serial.print(F("❌ Failed to get full configuration descriptor: "));
        Serial.println(rcode, HEX);
        free(fullDescBuf);
        return rcode;
    }

    Serial.print(F("📋 Configuration descriptor length: "));
    Serial.println(fullConfDescrLen);

    USB_CONFIGURATION_DESCRIPTOR fullConfDesc = *((USB_CONFIGURATION_DESCRIPTOR*)fullDescBuf);
    Serial.print(F("📋 bNumInterfaces: "));
    Serial.print(fullConfDesc.bNumInterfaces);
    Serial.print(F(", wTotalLength: "));
    Serial.println(fullConfDesc.wTotalLength);

    Serial.println("\n");

    // Parse descriptors to find endpoints
    uint8_t currentInterface = 255;
    uint8_t interface1InEndpoint = 0;
    uint8_t interface1OutEndpoint = 0;
    uint8_t controlInterface = 255;
    uint8_t controlInEndpoint = 0;
    uint8_t controlOutEndpoint = 0;

    for (uint16_t i = 0; i < fullConfDesc.wTotalLength; ) {
        uint8_t* p = fullDescBuf + i;
        
        if (p[1] == USB_DESCRIPTOR_INTERFACE) {
            currentInterface = p[2];
            Serial.print(F("🔍 Found interface: "));
            Serial.print(currentInterface);
            Serial.print(F(" Class: 0x"));
            Serial.print(p[5], HEX);
            Serial.print(F(" SubClass: 0x"));
            Serial.print(p[6], HEX);
            Serial.print(F(" Protocol: 0x"));
            Serial.println(p[7], HEX);
            
            // Look for control interface (class 255)
            if (p[5] == 255) {
                controlInterface = currentInterface;
                Serial.print(F("📍 Control interface: "));
                Serial.println(controlInterface);
            }
        }
        else if (p[1] == USB_DESCRIPTOR_ENDPOINT && currentInterface == 1) {
            // Detect endpoints for interface 1
            if (p[2] & 0x80) {
                interface1InEndpoint = p[2];
                Serial.print(F("📥 Interface 1 IN: 0x"));
                Serial.println(interface1InEndpoint, HEX);
            } else {
                interface1OutEndpoint = p[2];
                Serial.print(F("📤 Interface 1 OUT: 0x"));
                Serial.println(interface1OutEndpoint, HEX);
            }
        }
        else if (p[1] == USB_DESCRIPTOR_ENDPOINT && currentInterface == controlInterface) {
            // Also detect control interface endpoints for reference
            if (p[2] & 0x80) {
                controlInEndpoint = p[2];
                Serial.print(F("📥 Control IN: 0x"));
                Serial.println(controlInEndpoint, HEX);
            } else {
                controlOutEndpoint = p[2];
                Serial.print(F("📤 Control OUT: 0x"));
                Serial.println(controlOutEndpoint, HEX);
            }
        }
        
        i += p[0];
    }

    // Check if we found interface 1 endpoints
    if (interface1InEndpoint == 0 || interface1OutEndpoint == 0) {
        Serial.println(F("❌ Interface 1 endpoints not found"));
        free(fullDescBuf);
        return USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE;
    }

    // Use interface 1 endpoints for commands
    controlInEndpoint = interface1InEndpoint;
    controlOutEndpoint = interface1OutEndpoint;
    Serial.print(F("📍 Using Interface 1 endpoints: IN=0x"));
    Serial.print(controlInEndpoint, HEX);
    Serial.print(F(" OUT=0x"));
    Serial.println(controlOutEndpoint, HEX);
    
    // Set up endpoint information array like AMBX example
    epInfo[0].epAddr = 0;
    epInfo[0].maxPktSize = 8;
    epInfo[0].epAttribs = 0;
    epInfo[0].bmNakPower = USB_NAK_MAX_POWER;
    epInfo[0].bmSndToggle = 0;
    epInfo[0].bmRcvToggle = 0;
    
    // IN endpoint
    epInfo[1].epAddr = controlInEndpoint;
    epInfo[1].maxPktSize = 64;
    epInfo[1].epAttribs = USB_TRANSFER_TYPE_INTERRUPT;
    epInfo[1].bmNakPower = USB_NAK_NOWAIT;
    epInfo[1].bmSndToggle = 0;
    epInfo[1].bmRcvToggle = 0;
    
    // OUT endpoint  
    epInfo[2].epAddr = controlOutEndpoint;
    epInfo[2].maxPktSize = 64;
    epInfo[2].epAttribs = USB_TRANSFER_TYPE_INTERRUPT;
    epInfo[2].bmNakPower = USB_NAK_NOWAIT;
    epInfo[2].bmSndToggle = 0;
    epInfo[2].bmRcvToggle = 0;
    
    Serial.println(F("📍 Pipe configuration:"));
    Serial.print(F("  Control: addr=0 maxPkt=8"));
    Serial.println();
    Serial.print(F("  IN: addr=0x"));
    Serial.print(controlInEndpoint, HEX);
    Serial.print(F(" maxPkt=64"));
    Serial.println();
    Serial.print(F("  OUT: addr=0x"));
    Serial.print(controlOutEndpoint, HEX);
    Serial.print(F(" maxPkt=64"));
    Serial.println();
    
    Serial.println(F("✅ Endpoint discovery complete - initialization will be handled by USB Host Shield Library"));
    free(fullDescBuf);
    
    // Note: The USB Host Shield Library will automatically call our Init() callback
    // when it detects our device, so we don't need to manually manage endpoints here.
    // This function is kept for backwards compatibility but the real initialization
    // happens in the Init(parent, port, lowspeed) callback.
    
    Serial.println(F("✅ initializeDevice complete - awaiting USB Host Shield Library callbacks"));
    initialized = true;
    return 0;
}

