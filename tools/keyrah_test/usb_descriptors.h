// usb_descriptors.h - keyrah_test
//
// Interface layout matches the real Keyrah V3: 2 boot-protocol keyboard
// interfaces, 2 joystick-shaped generic-HID interfaces, and 2 more
// generic-HID interfaces carrying "Multimedia Control" and "ACPI Power
// Control" usages. A CDC interface is added on top for this mock's own test
// console - Circle binds interfaces purely by class/subclass/protocol, so
// the extra interface doesn't change how the 6 HID ones are seen.

#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include "tusb.h"

enum {
  ITF_KBD1 = 0,   // boot keyboard -> Circle ukbd1
  ITF_KBD2,       // boot keyboard -> Circle ukbd2
  ITF_JOY1,       // generic HID   -> Circle upad1
  ITF_JOY2,       // generic HID   -> Circle upad2
  ITF_CONSUMER,   // generic HID, Consumer Control usage
  ITF_SYSCTRL,    // generic HID, System Control usage
  ITF_NUM_HID_TOTAL,
};

enum {
  ITF_CDC = ITF_NUM_HID_TOTAL,  // uses ITF_CDC and ITF_CDC+1 internally (TUD_CDC_DESCRIPTOR)
  ITF_NUM_TOTAL = ITF_CDC + 2,
};

// bInterval (ms, full-speed) applied to every HID interrupt-IN endpoint. Set
// via CMakeLists.txt target_compile_definitions; defaults to 1 (1000Hz,
// matching the real Keyrah's descriptors) if not overridden.
#ifndef KEYRAH_POLL_INTERVAL_MS
#define KEYRAH_POLL_INTERVAL_MS   1
#endif

// wMaxPacketSize for the keyboard/joystick endpoints. 8 bytes matches the
// boot report size and what Circle's CUSBKeyboardDevice expects
// (USBKEYB_REPORT_SIZE), rather than the real Keyrah's 48.
#define HID_EP_SIZE               8

#endif
