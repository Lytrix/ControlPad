#include "CMControlPad.h"

uint8_t CMControlPad::Init(uint8_t parent, uint8_t port, bool lowspeed) {
    uint8_t buf[sizeof(USB_DEVICE_DESCRIPTOR)];
    USB_DEVICE_DESCRIPTOR* udd = reinterpret_cast<USB_DEVICE_DESCRIPTOR*>(buf);
    uint8_t rcode;
    uint8_t num_of_conf;
    
    // USB Speed Detection and Reporting
    Serial.println(F("🚀 USB Speed Detection:"));
    Serial.print(F("  lowspeed parameter: "));
    Serial.println(lowspeed ? F("LOW SPEED (1.5 Mbps)") : F("FULL SPEED (12 Mbps)"));
    
    // Check actual USB host speed configuration
    // Note: USB Host Shield Library should auto-detect speed during enumeration
    Serial.println(F("  Expected for CM Control Pad: FULL SPEED (12 Mbps)"));
    
    // Read MAX3421E MODE register to check speed configuration
    uint8_t modeReg = pUsb->regRd(rMODE);
    Serial.print(F("📍 MAX3421E MODE register: 0x"));
    Serial.println(modeReg, HEX);
    Serial.print(F("  LOWSPEED bit (bit 2): "));
    Serial.println((modeReg & bmLOWSPEED) ? F("SET (Low Speed)") : F("CLEAR (Full Speed)"));
    Serial.print(F("  HOST bit (bit 0): "));
    Serial.println((modeReg & bmHOST) ? F("SET (Host Mode)") : F("CLEAR (Peripheral Mode)"));
    
    if (lowspeed) {
        Serial.println(F("⚠️  WARNING: Device detected as LOW SPEED!"));
        Serial.println(F("   This could cause timing and communication issues."));
        Serial.println(F("   CM Control Pad should be FULL SPEED device."));
    } else {
        Serial.println(F("✅ Device detected as FULL SPEED - correct for CM Control Pad"));
    }
    
    // Force full speed mode if needed
    if (modeReg & bmLOWSPEED) {
        Serial.println(F("🔧 Forcing MAX3421E to FULL SPEED mode..."));
        pUsb->regWr(rMODE, modeReg & ~bmLOWSPEED);  // Clear LOWSPEED bit
        delay(10);  // Allow register to update
        uint8_t newModeReg = pUsb->regRd(rMODE);
        Serial.print(F("📍 Updated MODE register: 0x"));
        Serial.println(newModeReg, HEX);
    }
    
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
    uint8_t interface0InEndpoint = 0;
    uint8_t interface0OutEndpoint = 0;
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
        else if (p[1] == USB_DESCRIPTOR_ENDPOINT && currentInterface == 0) {
            // Detect endpoints for interface 0 (HID keyboard)
            if (p[2] & 0x80) {
                interface0InEndpoint = p[2];
                Serial.print(F("📥 Interface 0 IN: 0x"));
                Serial.println(interface0InEndpoint, HEX);
            } else {
                interface0OutEndpoint = p[2];
                Serial.print(F("📤 Interface 0 OUT: 0x"));
                Serial.println(interface0OutEndpoint, HEX);
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

    // Report what we found
    Serial.println(F("\n📋 Endpoint Summary:"));
    Serial.print(F("  Interface 0 (HID Keyboard): IN=0x"));
    Serial.print(interface0InEndpoint, HEX);
    Serial.print(F(" OUT=0x"));
    Serial.println(interface0OutEndpoint, HEX);
    Serial.print(F("  Interface 1 (Control): IN=0x"));
    Serial.print(interface1InEndpoint, HEX);
    Serial.print(F(" OUT=0x"));
    Serial.println(interface1OutEndpoint, HEX);

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
    delay(200);
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
    
    // Activate Interface 0 (HID Keyboard Interface)
    Serial.println(F("🔧 Activating Interface 0 (HID Keyboard)..."));
    
    // Use HID SET_IDLE request instead of SET_INTERFACE for keyboard interface
    // bmRequestType: 0x21 (Host to Device, Class, Interface)
    // bRequest: HID_SET_IDLE (0x0A)
    // wValue: (Duration << 8) | Report ID (0x0000 = indefinite idle)
    // wIndex: Interface Number (0)
    // wLength: 0
    rcode = pUsb->ctrlReq(bAddress, 0, 0x21, 0x0A, 0x0000, 0, 0, 0, 0, NULL, NULL);
    if (rcode) {
        Serial.print(F("⚠️ HID SET_IDLE for Interface 0 failed: "));
        Serial.println(rcode, HEX);
        Serial.println(F("   Continuing anyway - will try alternative activation"));
    } else {
        Serial.println(F("✅ Interface 0 (HID) SET_IDLE successful"));
    }
    
    // Also try HID SET_PROTOCOL for keyboard interface
    Serial.println(F("🔧 Setting HID Protocol for Interface 0..."));
    // bmRequestType: 0x21 (Host to Device, Class, Interface)  
    // bRequest: HID_SET_PROTOCOL (0x0B)
    // wValue: Protocol (1 = Report Protocol, 0 = Boot Protocol)
    // wIndex: Interface Number (0)
    rcode = pUsb->ctrlReq(bAddress, 0, 0x21, 0x0B, 1, 0, 0, 0, 0, NULL, NULL);
    if (rcode) {
        Serial.print(F("⚠️ HID SET_PROTOCOL for Interface 0 failed: "));
        Serial.println(rcode, HEX);
    } else {
        Serial.println(F("✅ Interface 0 (HID) SET_PROTOCOL successful"));
    }
    
    // Activate Interface 1 (Control Interface)
    Serial.println(F("🔧 Activating Interface 1 (Control Interface)..."));
    
    // Use control transfer to set interface 1
    rcode = pUsb->ctrlReq(bAddress, 0, bmREQ_SET, USB_REQUEST_SET_INTERFACE, 0, 1, 0, 0, 0, NULL, NULL);
    if (rcode) {
        Serial.print(F("⚠️ Set Interface 1 failed: "));
        Serial.println(rcode, HEX);
        Serial.println(F("   Continuing anyway - some devices don't support SET_INTERFACE"));
    } else {
        Serial.println(F("✅ Interface 1 activated"));
    }
    
    // Small delay for interface activation to take effect
    delay(100);
    
    free(fullDescBuf);
    
    // Send activation sequence using proper pipe management
    Serial.println(F("🔧 Starting activation sequence..."));
    bool success = true;

    // Initialize data toggle tracking for debugging
    Serial.println(F("📍 Initial data toggle states:"));
    Serial.print(F("  OUT endpoint bmSndToggle: "));
    Serial.println(epInfo[2].bmSndToggle);
    Serial.print(F("  IN endpoint bmRcvToggle: "));
    Serial.println(epInfo[1].bmRcvToggle);

    // Step 1: 0x42 00 activation (part 1 of multi-packet)
    Serial.println(F("📤 Step 1: 0x42 00 activation"));
    uint8_t cmd1[64] = {0};
    cmd1[0] = 0x42; cmd1[1] = 0x00; cmd1[4] = 0x01; cmd1[7] = 0x01;
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd1);
    Serial.println(rcode);
    if (rcode) {
        Serial.print(F("❌ Step 1 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 1 sent"));
        
        // Read data toggle state after transfer (for debugging)
        Serial.print(F("📍 Step 1 OUT toggle after transfer: "));
        Serial.println(epInfo[2].bmSndToggle);
        
        // Frame-synchronized polling
        pUsb->Task();
        delay(1); // 1ms USB frame timing
        pUsb->Task();
        delayMicroseconds(500); // Small delay for device to respond
        uint8_t response[64];
        uint16_t len = 64;
        uint8_t readCode = pUsb->inTransfer(bAddress, epInfo[1].epAddr, &len, response);
        if (readCode == 0 && len > 0) {
            Serial.print(F("📨 Step 1 response: "));
            for (uint8_t i = 0; i < len; i++) {
                Serial.print(response[i], HEX);
                Serial.print(F(" "));
            }
            Serial.println();
            // Read IN toggle state after response
            Serial.print(F("📍 Step 1 IN toggle after response: "));
            Serial.println(epInfo[1].bmRcvToggle);
        }
    }
    delay(1); // USB frame boundary timing

    // Step 2: 0x42 10 variant (part 2 of multi-packet)
    Serial.println(F("📤 Step 2: 0x42 10 variant"));
    uint8_t cmd2[64] = {0};
    cmd2[0] = 0x42; cmd2[1] = 0x10; cmd2[4] = 0x01; cmd2[7] = 0x01;
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd2);
    Serial.println(rcode);
    if (rcode) {
        Serial.print(F("❌ Step 2 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 2 sent"));
        
        // Read data toggle state after transfer (for debugging)
        Serial.print(F("📍 Step 2 OUT toggle after transfer: "));
        Serial.println(epInfo[2].bmSndToggle);
        
        // Frame-synchronized polling
        pUsb->Task();
        delay(1); // 1ms USB frame timing
        pUsb->Task();
        delayMicroseconds(500); // Small delay for device to respond
        uint8_t response[64];
        uint16_t len = 64;
        uint8_t readCode = pUsb->inTransfer(bAddress, epInfo[1].epAddr, &len, response);
        if (readCode == 0 && len > 0) {
            Serial.print(F("📨 Step 2 response: "));
            for (uint8_t i = 0; i < len; i++) {
                Serial.print(response[i], HEX);
                Serial.print(F(" "));
            }
            Serial.println();
            // Read IN toggle state after response
            Serial.print(F("📍 Step 2 IN toggle after response: "));
            Serial.println(epInfo[1].bmRcvToggle);
        }
        
    }
    delay(1); // USB frame boundary timing

    // Step 3: 0x43 00 button activation
    Serial.println(F("📤 Step 3: 0x43 00 button activation"));
    uint8_t cmd3[64] = {0};
    cmd3[0] = 0x43; cmd3[1] = 0x00; cmd3[4] = 0x01;
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd3);
    Serial.println(rcode);
    if (rcode) {
        Serial.print(F("❌ Step 3 failed: 0x"));
        Serial.println(rcode, HEX);
        success = false;
    } else {
        Serial.println(F("✅ Step 3 sent"));
        
        // Read data toggle state after transfer (for debugging)
        Serial.print(F("📍 Step 3 OUT toggle after transfer: "));
        Serial.println(epInfo[2].bmSndToggle);
        
        // Frame-synchronized polling
        pUsb->Task();
        delay(1); // 1ms USB frame timing
        pUsb->Task();
        delayMicroseconds(500); // Small delay for device to respond
        uint8_t response[64];
        uint16_t len = 64;
        uint8_t readCode = pUsb->inTransfer(bAddress, epInfo[1].epAddr, &len, response);
        if (readCode == 0 && len > 0) {
            Serial.print(F("📨 Step 3 response: "));
            for (uint8_t i = 0; i < len; i++) {
                Serial.print(response[i], HEX);
                Serial.print(F(" "));
            }
            Serial.println();
            // Read IN toggle state after response
            Serial.print(F("📍 Step 3 IN toggle after response: "));
            Serial.println(epInfo[1].bmRcvToggle);
        }
         
    }
    
    // Check data toggle states before problematic commands
    Serial.println(F("📍 Data toggle states before problematic commands:"));
    Serial.print(F("  OUT endpoint bmSndToggle: "));
    Serial.println(epInfo[2].bmSndToggle);
    Serial.print(F("  IN endpoint bmRcvToggle: "));
    Serial.println(epInfo[1].bmRcvToggle);
    
    // DON'T clear endpoint state - preserve data toggle synchronization
    Serial.println(F("🔄 Preserving endpoint data toggle state..."));
    
    // Device state recovery after Step 3
    Serial.println(F("🕒 Device state recovery sequence..."));
    
    // Give device more time to process Step 3 internally
    for (uint8_t recovery = 0; recovery < 10; recovery++) {
        pUsb->Task();
        delay(2); // Extended timing for device internal processing
    }
    
    // Try to "wake up" the device with a gentle poll
    Serial.println(F("📡 Gentle device polling..."));
    uint8_t wakeupData[64];
    uint16_t wakeupLen = 64;
    pUsb->inTransfer(bAddress, epInfo[1].epAddr, &wakeupLen, wakeupData);
    pUsb->Task();
    delay(5);
    
    Serial.print(F("📍 Next OUT command should use DATA"));
    Serial.println(epInfo[2].bmSndToggle == 1 ? "0" : "1");
    
    // EXPERIMENTAL: Force toggle reset to DATA0 for second phase commands
    // The device seems to expect toggle reset after the 3-command initialization
    Serial.println(F("🔧 Forcing toggle reset to DATA0 for second phase..."));
    epInfo[2].bmSndToggle = 0;  // Force DATA0 for next transfer
    
    // Update endpoint info in USB Host Shield Library
    uint8_t toggleResetResult = pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    if (toggleResetResult) {
        Serial.print(F("⚠️ Toggle reset failed: 0x"));
        Serial.println(toggleResetResult, HEX);
    } else {
        Serial.println(F("✅ Toggle reset to DATA0 successful"));
    }
    
    delay(1); // Frame boundary

    // Step 4: 0x41 80 status (check device state first)
    Serial.println(F("📤 Step 4: 0x41 80 status"));
    Serial.print(F("📍 Step 4 OUT toggle before transfer: "));
    Serial.println(epInfo[2].bmSndToggle);
    uint8_t cmd4[64] = {0};
    cmd4[0] = 0x41; cmd4[1] = 0x80;
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd4);
    if (rcode) {
        Serial.print(F("❌ Step 4 failed: 0x"));
        Serial.println(rcode, HEX);
        Serial.print(F("📍 Step 4 OUT toggle after failed transfer: "));
        Serial.println(epInfo[2].bmSndToggle);
        success = false;
    } else {
        Serial.println(F("✅ Step 4 sent"));
        // Frame-synchronized polling
        pUsb->Task();
        delay(1); // 1ms USB frame timing
        pUsb->Task();
        delayMicroseconds(500); // Small delay for device to respond
        uint8_t response[64];
        uint16_t len = 64;
        uint8_t readCode = pUsb->inTransfer(bAddress, epInfo[1].epAddr, &len, response);
        if (readCode == 0 && len > 0) {
            Serial.print(F("📨 Step 4 response: "));
            for (uint8_t i = 0; i < len; i++) {
                Serial.print(response[i], HEX);
                Serial.print(F(" "));
            }
            Serial.println();
        }
      
    }
    delay(1); // USB frame boundary timing

    // Effect mode activation might also need toggle reset 
    Serial.println(F("🔧 Toggle reset before effect mode activation..."));
    epInfo[2].bmSndToggle = 0;  // Force DATA0 for effect mode command
    
    // Update endpoint info again
    uint8_t effectToggleReset = pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    if (effectToggleReset) {
        Serial.print(F("⚠️ Effect toggle reset failed: 0x"));
        Serial.println(effectToggleReset, HEX);
    } else {
        Serial.println(F("✅ Effect toggle reset successful"));
    }

    // Step 5: 0x52 00 activate effect modes (after status check)
    Serial.println(F("📤 Step 5: 0x52 00 activate effect modes"));
    Serial.print(F("📍 Step 5 OUT toggle before transfer: "));
    Serial.println(epInfo[2].bmSndToggle);
    uint8_t cmd5[64] = {0};
    cmd5[0] = 0x52; cmd5[1] = 0x00;
    rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd5);
    if (rcode) {
        Serial.print(F("❌ Step 5 failed: 0x"));
        Serial.println(rcode, HEX);
        Serial.print(F("📍 Step 5 OUT toggle after failed transfer: "));
        Serial.println(epInfo[2].bmSndToggle);
        success = false;
    } else {
        Serial.println(F("✅ Step 5 sent"));
        // Frame-synchronized polling
        pUsb->Task();
        delay(1); // 1ms USB frame timing
        pUsb->Task();
        delayMicroseconds(500); // Small delay for device to respond
        uint8_t response[64];
        uint16_t len = 64;
        uint8_t readCode = pUsb->inTransfer(bAddress, epInfo[1].epAddr, &len, response);
        if (readCode == 0 && len > 0) {
            Serial.print(F("📨 Step 5 response: "));
            for (uint8_t i = 0; i < len; i++) {
                Serial.print(response[i], HEX);
                Serial.print(F(" "));
            }
            Serial.println();
        }
        
    }
    delay(1); // USB frame boundary timing

    if (success) {
        Serial.println(F("✅ Activation sequence completed successfully"));
    } else {
        Serial.println(F("⚠️ Activation sequence had failures"));
    }

    Serial.println(F("✅ Init complete"));
    delay(500);

    Serial.println(F("🎉 CM Control Pad initialization completed successfully!"));
    initialized = true;
    bPollEnable = true;
    
    // Set custom mode once after initialization
    delay(10); // Small delay after initialization
    setCustomMode();
    
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
    uint8_t interface0InEndpoint = 0;
    uint8_t interface0OutEndpoint = 0;
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
        else if (p[1] == USB_DESCRIPTOR_ENDPOINT && currentInterface == 0) {
            // Detect endpoints for interface 0 (HID keyboard)
            if (p[2] & 0x80) {
                interface0InEndpoint = p[2];
                Serial.print(F("📥 Interface 0 IN: 0x"));
                Serial.println(interface0InEndpoint, HEX);
            } else {
                interface0OutEndpoint = p[2];
                Serial.print(F("📤 Interface 0 OUT: 0x"));
                Serial.println(interface0OutEndpoint, HEX);
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

    // Report what we found
    Serial.println(F("\n📋 Endpoint Summary:"));
    Serial.print(F("  Interface 0 (HID Keyboard): IN=0x"));
    Serial.print(interface0InEndpoint, HEX);
    Serial.print(F(" OUT=0x"));
    Serial.println(interface0OutEndpoint, HEX);
    Serial.print(F("  Interface 1 (Control): IN=0x"));
    Serial.print(interface1InEndpoint, HEX);
    Serial.print(F(" OUT=0x"));
    Serial.println(interface1OutEndpoint, HEX);

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

bool CMControlPad::setCustomMode() {
    Serial.println(F("🎨 setCustomMode: Setting device to custom LED mode (CORRECT FORMAT)..."));
    
    // COMMAND 1: Set effect mode (from user documentation)
    // 56 81 0000 01000000 02000000 bbbbbbbb (custom mode) + trailing zeros
    uint8_t cmd[64] = {0};
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
    // Rest remains zeros
    
    Serial.print(F("🎨 Custom mode command: 0x"));
    Serial.print(cmd[0], HEX);
    Serial.print(F(" 0x"));
    Serial.print(cmd[1], HEX);
    Serial.println(F(" (CORRECT custom mode setup)"));
    
    // Reset toggle for new command type
    epInfo[2].bmSndToggle = 0;  // Force DATA0
    uint8_t toggleReset = pUsb->setEpInfoEntry(bAddress, 3, epInfo);
    if (toggleReset) {
        Serial.print(F("⚠️ Custom mode toggle reset failed: 0x"));
        Serial.println(toggleReset, HEX);
    }
    
    uint8_t rcode = pUsb->outTransfer(bAddress, epInfo[2].epAddr, 64, cmd);
    if (rcode) {
        Serial.print(F("❌ setCustomMode failed: 0x"));
        Serial.println(rcode, HEX);
        return false;
    }
    
    Serial.println(F("🎨 setCustomMode result: SUCCESS"));
    return true;
}


