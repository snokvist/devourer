#ifndef MT7612U_USB_IDS_H
#define MT7612U_USB_IDS_H

#include <cstdint>

namespace mt7612u {

/* MediaTek dispatch is VID:PID only, and has to happen BEFORE the Realtek
 * SYS_CFG2 read in WiFiDriver::CreateRtlDevice.
 *
 * 0x00FC is a Realtek register. On MT7612U it is inside the MAC register
 * window, and reaching it needs a MediaTek vendor request (MT_VEND_MULTI_READ
 * 0x07 with the address split across wValue/wIndex) rather than the Realtek
 * REALTEK_USB_VENQT_READ. Issuing the Realtek request against MediaTek silicon
 * reads nothing meaningful, so the byte it returns cannot be allowed to pick a
 * backend. Kestrel gates ahead of the same read for the same class of reason
 * (see the comment at its gate).
 *
 * The chip identity is confirmed after the handle is adopted, by reading
 * MT_ASIC_VERSION: 0x76120044 on the sample this port was built against.
 */
struct UsbId {
  uint16_t vid;
  uint16_t pid;
};

inline constexpr UsbId kUsbIds[] = {
    {0x0e8d, 0x7612}, /* MediaTek MT7612U reference identity */
    {0x0e8d, 0x7662}, /* MT7662U, the same MAC with a different strap */
};

inline bool is_usb_id(uint16_t vid, uint16_t pid) {
  for (const UsbId &id : kUsbIds)
    if (id.vid == vid && id.pid == pid)
      return true;
  return false;
}

} // namespace mt7612u

#endif /* MT7612U_USB_IDS_H */
