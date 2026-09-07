/*
 * usb_transport.h
 *
 * Transport abstraction for vhci::attach(). TcpTransport wraps the existing,
 * stable TCP attach path unchanged. The kernel VHCI driver owns URB I/O; this
 * interface keeps the attach operation isolated from the GUI.
 */

#pragma once

#include <vhci.h>

namespace usbip::transport
{

class IUsbTransport
{
public:
    virtual ~IUsbTransport() = default;

    // Attaches the device via the vhci driver, returning the hub port (>=1) or <1 on failure.
    virtual int connect(HANDLE dev, const usbip::device_location &location) = 0;
};

// Wraps the existing, stable TCP attach path. Behavior is identical to the pre-existing baseline.
class TcpTransport : public IUsbTransport
{
public:
    int connect(HANDLE dev, const usbip::device_location &location) override
    {
        return usbip::vhci::attach(dev, location);
    }

};


} // namespace usbip::transport
