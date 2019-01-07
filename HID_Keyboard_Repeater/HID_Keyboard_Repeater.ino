//-------------------------------------------------------------------------------------------//
#include <avr/pgmspace.h>
#include <avrpins.h>
#include <max3421e.h>
#include <usbhost.h>
#include <usb_ch9.h>
#include <Usb.h>
#include <usbhub.h>
#include <avr/pgmspace.h>
#include <address.h>
#include <hidboot.h>
#include <printhex.h>
#include <message.h>
#include <hexdump.h>
#include <parsetools.h>
//----------------------------------------//
class KbdRptParser : public KeyboardReportParser {
public:
    char Conv_OemToAscii(uint8_t mod, uint8_t key);
protected:
    virtual void OnKeyDown(uint8_t mod, uint8_t key);
};
void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key) {ProcessKey(mod,key);} // Send key to processor when key is down
char KbdRptParser::Conv_OemToAscii(uint8_t mod, uint8_t key) {return (char)OemToAscii(mod,key);} // Tap into protected function
//----------------------------------------//
// Usb / HID keyboard
USB Usb;
HIDBoot<HID_PROTOCOL_KEYBOARD> Keyboard(&Usb);
KbdRptParser Prs;
//-------------------------------------------------------------------------------------------//
// LEDs/Pins
const byte CaptureSwitchSig_pin   = 2; // INT0 (don't move)
const byte PlaybackSwitchSig_pin  = 3; // INT1 (don't move)
const byte CaptureReadyLED_pin    = A5;
const byte PlaybackRunningLED_pin = A3;
// Memory/Capture/Playback
#define MAX_KEYS 255    // Note that this is compared against a byte, so 255 is max for code
#define XmitDelay 50    // ms Delay between transmitted keys
volatile bool CaptureOn       = false;
volatile bool PlaybackEnabled = false;
volatile uint8_t CaptureIndex = 0;
bool HaveCapturedAtLeastOne   = false;
uint8_t modCaptureBuffer[MAX_KEYS] = {0};
uint8_t keyCaptureBuffer[MAX_KEYS] = {0};
//-------------------------------------------------------------------------------------------//
void setup() {
    Serial.begin(115200);
    // LED Pin modes (use adjacent pin as ground)
    pinMode(CaptureReadyLED_pin,OUTPUT);
    pinMode(CaptureReadyLED_pin-1,OUTPUT);
    pinMode(PlaybackRunningLED_pin,OUTPUT);
    pinMode(PlaybackRunningLED_pin-1,OUTPUT);
    // Pin init (and GND set)
    digitalWrite(CaptureReadyLED_pin,LOW);
    digitalWrite(CaptureReadyLED_pin-1,LOW);    // GND
    digitalWrite(PlaybackRunningLED_pin,LOW);
    digitalWrite(PlaybackRunningLED_pin-1,LOW); // GND
    // Switches PWR/GND (Sig vals seen by INTR)
    pinMode(4,OUTPUT);
    pinMode(5,OUTPUT);
    pinMode(6,OUTPUT);
    pinMode(7,OUTPUT);
    digitalWrite(4,LOW);
    digitalWrite(5,HIGH);
    digitalWrite(6,HIGH);
    digitalWrite(7,LOW);
    attachInterrupt(0, CaptureSwitchISR, FALLING); // Falling edge because switch is on-off-on
    attachInterrupt(1, PlaybackSwitchISR, RISING); // Rising edge because switch is off-on-off
    // Init USB/Keyboard
    if (Usb.Init() == -1) FreezeInErrorState();
    delay(200);
    Keyboard.SetReportParser(0,(HIDReportParser*)&Prs);
    // Blink LEDs to show On
    for(uint8_t i=0;i<7;i++) {
        digitalWrite(CaptureReadyLED_pin,   (i%2)?HIGH:LOW);
        digitalWrite(PlaybackRunningLED_pin,(i%2)?HIGH:LOW);
        delay(500);
    }
}
//-------------------------------------------------------------------------------------------//
void CaptureSwitchISR() {
    // Read value again to be sure
    if(digitalRead(CaptureSwitchSig_pin)==0) { // Switch is on-off-on, should fall to zero
        HaveCapturedAtLeastOne = true;  // So playback is possible
        CaptureOn = !CaptureOn;         // Toggle on ISR call
        digitalWrite(CaptureReadyLED_pin,CaptureOn?HIGH:LOW); // Update LED status
    }
}
//-------------------------------------------------------------------------------------------//
void PlaybackSwitchISR() {
    // Read value again to be sure
    if((digitalRead(PlaybackSwitchSig_pin)!=0)&&HaveCapturedAtLeastOne) { // Switch is off-on-off, should rise to one
        if(!CaptureOn) {
            PlaybackEnabled = true;  // Flag for polling (can't use delay in ISR)
            digitalWrite(PlaybackRunningLED_pin,HIGH);
        } else {
            CaptureIndex = 0; // Reset buffer index
            // Blink Capture LED (ending with HIGH)
            for(uint8_t i=0;i<8;i++) {
                for(uint16_t j=0;j<41668;j++) { // Can't use delay() in ISRs
                    digitalWrite(CaptureReadyLED_pin,(i%2)?HIGH:LOW); //(41667*(3us + ~3us) = 250ms
                }
            }
        }
    }
}
//-------------------------------------------------------------------------------------------//
void loop() {
    Usb.Task();
    if(PlaybackEnabled) PlayCapSeq(); // Playback poll:
    PlaybackEnabled = false;          // Playback once guarentee
}
//-------------------------------------------------------------------------------------------//
void ProcessKey(uint8_t mod, uint8_t key) {
    if(!CaptureOn) { // Just repeate
        RepeateKey(mod,key);
    } else {         // Capture
        if(CaptureIndex == MAX_KEYS) {
            CaptureIndex=0;
            // Blink Capture LED (ending with HIGH)
            for(uint8_t i=0;i<8;i++) {
                digitalWrite(CaptureReadyLED_pin,(i%2)?HIGH:LOW);
                delay(250);
            }
        }
        // Capture
        modCaptureBuffer[CaptureIndex] = mod;
        keyCaptureBuffer[CaptureIndex] = key;
        CaptureIndex++;
    }
}
//-------------------------------------------------------------------------------------------//
void PlayCapSeq() {
    uint8_t i;
    // Flicker before sending (ending with HIGH)
    for(i=0;i<20;i++) {
        digitalWrite(PlaybackRunningLED_pin,(i%2)?HIGH:LOW);
        delay(100);
    }
    // Send Buffer
    for(i=0;i < CaptureIndex;i++) {
        RepeateKey(modCaptureBuffer[i],keyCaptureBuffer[i]);
        delay(XmitDelay); // As to not overwhelm (may need to be longer if certian CMDs take longer than 50ms)
    }
    // Flicker after sending (ending with LOW)
    for(i=0;i<21;i++) {
        digitalWrite(PlaybackRunningLED_pin,(i%2)?HIGH:LOW);
        delay(100);
    }
}
//-------------------------------------------------------------------------------------------//
void RepeateKey(uint8_t mod, uint8_t key) {
    uint8_t buf[8] = {0}; // 8 Char seq, I only know 0-mod,1-protected,2-7 keycode (that's pretty much just [2])
    // Key Press
    buf[0] = mod;
    buf[2] = key;
    Serial.write(buf, 8); // Write out buffer
    // Key Release
    buf[0] = 0;
    buf[2] = 0;
    Serial.write(buf, 8); // Write out buffer
    // Debug Send
    // char toSend = Prs.Conv_OemToAscii(mod,key); // Convert KB Boot Code to Ascii
    // Serial.write(toSend);
}
//-------------------------------------------------------------------------------------------//
void FreezeInErrorState() {
    digitalWrite(CaptureReadyLED_pin, HIGH);
    digitalWrite(PlaybackRunningLED_pin, HIGH);
    while(true) {}
}
//-------------------------------------------------------------------------------------------//