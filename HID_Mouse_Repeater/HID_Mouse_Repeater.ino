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
struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;   /* Not yet implemented */
} mouseReport;
//----------------------------------------//
class MouseRptParser : public MouseReportParser {
protected:
    virtual void OnMouseMove        (MOUSEINFO* mi);
    virtual void OnLeftButtonUp     (MOUSEINFO* mi);
    virtual void OnLeftButtonDown   (MOUSEINFO* mi);
    virtual void OnRightButtonUp    (MOUSEINFO* mi);
    virtual void OnRightButtonDown  (MOUSEINFO* mi);
    virtual void OnMiddleButtonUp   (MOUSEINFO* mi);
    virtual void OnMiddleButtonDown (MOUSEINFO* mi);
};
void MouseRptParser::OnMouseMove(MOUSEINFO* mi)        {ProcessDelta(mi);}
void MouseRptParser::OnLeftButtonUp(MOUSEINFO* mi)     {ProcessDelta(mi);}
void MouseRptParser::OnLeftButtonDown(MOUSEINFO* mi)   {ProcessDelta(mi);}
void MouseRptParser::OnRightButtonUp(MOUSEINFO* mi)    {ProcessDelta(mi);}
void MouseRptParser::OnRightButtonDown(MOUSEINFO* mi)  {ProcessDelta(mi);}
void MouseRptParser::OnMiddleButtonUp(MOUSEINFO* mi)   {ProcessDelta(mi);}
void MouseRptParser::OnMiddleButtonDown(MOUSEINFO* mi) {ProcessDelta(mi);}
//----------------------------------------//
// Usb / HID mouse
USB Usb;
HIDBoot<HID_PROTOCOL_MOUSE> Mouse(&Usb);
MouseRptParser Prs;
//-------------------------------------------------------------------------------------------//
// LEDs/Pins
const byte CaptureSwitchSig_pin   = 2; // INT0 (don't move)
const byte PlaybackSwitchSig_pin  = 3; // INT1 (don't move)
const byte CaptureReadyLED_pin    = A5;
const byte PlaybackRunningLED_pin = A3;
// Memory/Capture/Playback
#define MAX_DELTAS 419
#define XmitDelay 10   // ms Delay between transmitted deltas (tuned)
const bool InvertAxis          = false;
volatile bool CaptureOn        = false;
volatile bool PlaybackEnabled  = false;
volatile uint16_t CaptureIndex = 0;
volatile bool CompressionMode  = false;
bool HaveCapturedAtLeastOne    = false;
MOUSEINFO CaptureBuffer[MAX_DELTAS] = {0};
//-------------------------------------------------------------------------------------------//
void setup() {
    Serial.begin(9600);
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
    attachInterrupt(1, PlaybackSwitchISR, CHANGE); // Rising edge because switch is off-on-off
    // Init USB/Mouse
    if (Usb.Init() == -1) FreezeInErrorState();
    delay(200);
    Mouse.SetReportParser(0,(HIDReportParser*)&Prs);
    // Blink LEDs to show On
    for(uint8_t i=0;i<7;i++) {
        digitalWrite(CaptureReadyLED_pin,   (i%2)?HIGH:LOW);
        digitalWrite(PlaybackRunningLED_pin,(i%2)?HIGH:LOW);
        delay(500);
    }
}
//-------------------------------------------------------------------------------------------//
void CaptureSwitchISR() {
    // Read value again to be sure and debounce
    bool SwitchKicked = true;
    for(uint8_t i=0;i<16;i++) {
        SwitchKicked &= (digitalRead(CaptureSwitchSig_pin)==0);// Switch is on-off-on, should fall to zero
    }
    if(SwitchKicked) {
        HaveCapturedAtLeastOne = true;  // So playback is possible
        CaptureOn = !CaptureOn;         // Toggle on ISR call
        digitalWrite(CaptureReadyLED_pin,CaptureOn?HIGH:LOW); // Update LED status
    }
}
//-------------------------------------------------------------------------------------------//
void PlaybackSwitchISR() {
    // Read value again to be sure and debounce
    bool SwitchKicked = true;
    for(uint8_t i=0;i<16;i++) {
        SwitchKicked &= (digitalRead(PlaybackSwitchSig_pin)!=0);// Switch is off-on-off, should rise to one
    }
    if(SwitchKicked&&HaveCapturedAtLeastOne) {
        if(!CaptureOn) {
            PlaybackEnabled = true;  // Flag for polling (can't use delay in ISR)
            digitalWrite(PlaybackRunningLED_pin,HIGH);
        } else {   // Reset buffer index OR toggle CompressionMode
            if(CaptureIndex == 0) {
                CompressionMode = !CompressionMode;
                if(CompressionMode) BlinkLED(PlaybackRunningLED_pin,11); // Odd -> end with LOW
                else                BlinkLED(PlaybackRunningLED_pin,5);  // Odd -> end with LOW
            } else { // Reset buffer index
                CaptureIndex = 0;
                BlinkLED(CaptureReadyLED_pin,8); // Even -> end with HIGH
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
void ProcessDelta(MOUSEINFO* Info) {
    if(CaptureOn) {
        if(CaptureIndex == MAX_DELTAS) {
            CaptureIndex=0;
            // Blink Capture LED (ending with HIGH)
            for(uint8_t i=0;i<8;i++) {
                digitalWrite(CaptureReadyLED_pin,(i%2)?HIGH:LOW);
                delay(250);
            }
        }
        // Capture  (Lets hope the compiler will fix this lazy coding and use some kind of temp)
        if((CompressionMode)&&(CaptureIndex!=0)&&
           (CaptureBuffer[CaptureIndex-1].bmLeftButton   == Info->bmLeftButton)&&
           (CaptureBuffer[CaptureIndex-1].bmRightButton  == Info->bmRightButton)&&
           (CaptureBuffer[CaptureIndex-1].bmMiddleButton == Info->bmMiddleButton)) {  
            // Capture compressed
            CaptureBuffer[CaptureIndex-1].dX += Info->dX;
            CaptureBuffer[CaptureIndex-1].dY += Info->dY;
        } else {
            CaptureBuffer[CaptureIndex].dX             = Info->dX;
            CaptureBuffer[CaptureIndex].dY             = Info->dY;
            CaptureBuffer[CaptureIndex].bmLeftButton   = Info->bmLeftButton;
            CaptureBuffer[CaptureIndex].bmRightButton  = Info->bmRightButton;
            CaptureBuffer[CaptureIndex].bmMiddleButton = Info->bmMiddleButton;
            CaptureIndex++;
        }
    }
    RepeateDelta(Info); // Always repeate (it's hard to know where the pointer will end up)
}
//-------------------------------------------------------------------------------------------//
void PlayCapSeq() {
   uint16_t i;
   // Flicker before sending (ending with HIGH)
   for(i=0;i<20;i++) {
       digitalWrite(PlaybackRunningLED_pin,(i%2)?HIGH:LOW);
       delay(100);
   }
   // Send Buffer (delay built in)
   for(i=0;i < CaptureIndex;i++) RepeateDelta(&CaptureBuffer[i]);
   // Flicker after sending (ending with LOW)
   for(i=0;i<21;i++) {
       digitalWrite(PlaybackRunningLED_pin,(i%2)?HIGH:LOW);
       delay(100);
   }
}
//-------------------------------------------------------------------------------------------//
void RepeateDelta(MOUSEINFO* Info) {
    mouseReport.buttons = Info->bmLeftButton | ((Info->bmRightButton)<<1) | ((Info->bmMiddleButton)<<2);
    mouseReport.x = (InvertAxis)? -Info->dX : Info->dX;
    mouseReport.y = (InvertAxis)? -Info->dY : Info->dY;
    mouseReport.wheel = 0; // Not yet implemented in HEX!
    Serial.write((uint8_t*)&mouseReport, 4);
    delay(XmitDelay);
}
//-------------------------------------------------------------------------------------------//
void BlinkLED(uint8_t pin, uint8_t count) {   // Blink w/o the delay function (used in the ISRs above)
    for(uint8_t i=0;i<count;i++) {
        for(uint16_t j=0;j<41668;j++) {
            digitalWrite(pin,(i%2)?HIGH:LOW); //(41667*(3us + ~3us) = 250ms
        }
    }
}
//-------------------------------------------------------------------------------------------//
void FreezeInErrorState() {
    digitalWrite(CaptureReadyLED_pin, HIGH);
    digitalWrite(PlaybackRunningLED_pin, HIGH);
    while(true) {}
}
//-------------------------------------------------------------------------------------------//