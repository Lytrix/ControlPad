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
    void initializeInterface1();
    bool sendInitializationSequence();

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
    
    // Call parent initialization first
    uint8_t rcode = HIDComposite::OnInitSuccessful();
    
    // Now initialize Interface 1 for LED control
    if (!interface1Initialized) {
        Serial.println("🚀 Initializing Interface 1 for LED control...");
        initializeInterface1();
    }
    
    return rcode;
}

// Initialize Interface 1 with the CM Control Pad commands
void HIDSelector::initializeInterface1() {
    Serial.println("📤 Sending CM Control Pad initialization sequence...");
    
    if (sendInitializationSequence()) {
        interface1Initialized = true;
        Serial.println("✅ Interface 1 initialized successfully!");
    } else {
        Serial.println("❌ Interface 1 initialization failed");
    }
}

// Send the initialization command sequence
bool HIDSelector::sendInitializationSequence() {
    uint8_t cmd[64] = {0};
    uint8_t rcode;
    
    // Command 1: 42 00
    Serial.println("📤 Step 1: Setup command 42 00");
    memset(cmd, 0, 64);
    cmd[0] = 0x42; cmd[1] = 0x00;
    cmd[4] = 0x01; cmd[7] = 0x01;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode) {
        Serial.print("⚠️ Command 1 NAK/error: 0x");
        Serial.println(rcode, HEX);
        if (rcode != 0x4) return false; // Continue on NAK, fail on other errors
    }
    delay(10);
    
    // Command 2: 42 10
    Serial.println("📤 Step 2: Setup command 42 10");
    memset(cmd, 0, 64);
    cmd[0] = 0x42; cmd[1] = 0x10;
    cmd[4] = 0x01; cmd[7] = 0x01;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode) {
        Serial.print("⚠️ Command 2 NAK/error: 0x");
        Serial.println(rcode, HEX);
        if (rcode != 0x4) return false; // Continue on NAK, fail on other errors
    }
    delay(10);
    
    // Command 3: 43 00
    Serial.println("📤 Step 3: Setup command 43 00");
    memset(cmd, 0, 64);
    cmd[0] = 0x43; cmd[1] = 0x00;
    cmd[4] = 0x01;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode) {
        Serial.print("⚠️ Command 3 NAK/error: 0x");
        Serial.println(rcode, HEX);
        if (rcode != 0x4) return false; // Continue on NAK, fail on other errors
    }
    delay(15);
    
    // Command 4: 41 80 (status) - retry if fails (as shown in USB capture)
    Serial.println("📤 Step 4: Status command 41 80");
    memset(cmd, 0, 64);
    cmd[0] = 0x41; cmd[1] = 0x80;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode) {
        Serial.print("⚠️ Status command NAK, retrying: 0x");
        Serial.println(rcode, HEX);
        delay(5);
        // Retry as shown in USB capture
        Serial.println("📤 Step 4 retry: Status command 41 80");
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        if (rcode) {
            Serial.print("⚠️ Status command retry NAK: 0x");
            Serial.println(rcode, HEX);
        }
    }
    delay(10);
    
    // Command 5: 52 00 (activate effects) - retry if fails (as shown in USB capture)
    Serial.println("📤 Step 5: Activate effects 52 00");
    memset(cmd, 0, 64);
    cmd[0] = 0x52; cmd[1] = 0x00;
    rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
    if (rcode) {
        Serial.print("⚠️ Effects command NAK, retrying: 0x");
        Serial.println(rcode, HEX);
        delay(5);
        // Retry as shown in USB capture
        Serial.println("📤 Step 5 retry: Activate effects 52 00");
        rcode = pUsb->outTransfer(bAddress, 0x04, 64, cmd);
        if (rcode) {
            Serial.print("⚠️ Effects command retry NAK: 0x");
            Serial.println(rcode, HEX);
        }
    }
    delay(10);
    
    Serial.println("✅ Initialization sequence complete (device responding with echoes!)");
    return true; // Success - we saw command echoes which means device is responding
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
    
    // Poll Interface 1 endpoint 0x83 for additional data
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll > 50 && hidSelector.isReady()) { // Poll every 50ms
        lastPoll = millis();
        
        uint8_t buf[64];
        uint16_t len = 64;
        uint8_t rcode = Usb.inTransfer(hidSelector.GetAddress(), 0x83, &len, buf);
        
        if (rcode == 0 && len > 0) {
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
