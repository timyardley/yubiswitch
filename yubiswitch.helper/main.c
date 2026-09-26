/*
 yubiswitch - enable/disable yubikey
 Copyright (C) 2013-2015  Angelo "pallotron" Failla <pallotron@freaknet.org>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <syslog.h>
#include <os/log.h>
#include <xpc/xpc.h>

#define ylog(fmt, ...) os_log(OS_LOG_DEFAULT, fmt, ##__VA_ARGS__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Security/Authorization.h>


IOHIDManagerRef hidManager;
IOHIDDeviceRef hidDevice;
typedef struct {
    int vendorID;
    int productID;
    uint64_t registryID;
    uint32_t locationID;
    UInt8 configuration;
    Boolean valid;
} USBDeviceState;

static USBDeviceState usbState;
static Boolean desiredDisabled;
static int selectedVendorID;
static int selectedProductID;
static uint64_t requestGeneration;
static IONotificationPortRef usbNotifyPort;
static io_iterator_t usbAddedIterator;

// Power management notification port and notifier
static io_connect_t pmRootPort;
static IONotificationPortRef pmNotifyPort;
static io_object_t pmNotifier;

static void match_set(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

static int64_t usb_property_number(io_service_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key,
                                                       kCFAllocatorDefault, 0);
    int64_t number = 0;
    if (value != NULL) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number);
        }
        CFRelease(value);
    }
    return number;
}

// Keep the USB service identity so a restore cannot target another key with
// the same product ID. Location survives a USB service re-enumeration.
static io_service_t usb_service_find(int vendorID, int productID,
                                      const USBDeviceState *state) {
    CFMutableDictionaryRef matchDict = IOServiceMatching("IOUSBHostDevice");
    if (!matchDict) return 0;

    CFNumberRef vidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendorID);
    CFNumberRef pidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &productID);
    CFDictionarySetValue(matchDict, CFSTR("idVendor"), vidRef);
    CFDictionarySetValue(matchDict, CFSTR("idProduct"), pidRef);
    CFRelease(vidRef);
    CFRelease(pidRef);

    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matchDict,
                                     &iterator) != KERN_SUCCESS) return 0;
    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != 0) {
        if (state == NULL ||
            (state->locationID != 0 &&
             (uint32_t)usb_property_number(service,
                   CFSTR(kUSBDevicePropertyLocationID)) == state->locationID)) {
            IOObjectRelease(iterator);
            return service;
        }
        uint64_t registryID = 0;
        IORegistryEntryGetRegistryEntryID(service, &registryID);
        if (registryID == state->registryID) {
            IOObjectRelease(iterator);
            return service;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return 0;
}

static IOUSBDeviceInterface182 **usb_interface_create(io_service_t service) {
    IOCFPlugInInterface **plugIn = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
        &plugIn, &score);
    if (kr != kIOReturnSuccess || plugIn == NULL) {
        ylog("Failed to create USB plugin interface: 0x%x", kr);
        return NULL;
    }

    IOUSBDeviceInterface182 **dev = NULL;
    HRESULT result = (*plugIn)->QueryInterface(plugIn,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182), (LPVOID *)&dev);
    (*plugIn)->Release(plugIn);
    if (result != S_OK || dev == NULL) {
        ylog("Failed to obtain USB interface revision 182: 0x%x", (unsigned int)result);
        return NULL;
    }
    return dev;
}

static IOReturn usb_interface_open(IOUSBDeviceInterface182 **dev) {
    IOReturn result = (*dev)->USBDeviceOpenSeize(dev);
    if (result != kIOReturnSuccess) result = (*dev)->USBDeviceOpen(dev);
    return result;
}

static Boolean usb_device_configured(int vendorID, int productID) {
    io_service_t service = usb_service_find(vendorID, productID, NULL);
    if (service == 0) return true; // A disconnected key cannot remain in use.
    IOUSBDeviceInterface182 **dev = usb_interface_create(service);
    IOObjectRelease(service);
    if (dev == NULL) return false;
    UInt8 configuration = 0;
    IOReturn result = (*dev)->GetConfiguration(dev, &configuration);
    (*dev)->Release(dev);
    return result == kIOReturnSuccess && configuration != 0;
}

// Keep savedConfiguration until the requested configuration is confirmed.
// ResetDevice is deliberately avoided: it can re-enumerate the device while
// the helper still believes it is suspended.
static Boolean usb_device_enable(void) {
    if (!usbState.valid) return usb_device_configured(selectedVendorID,
                                                       selectedProductID);
    io_service_t service = usb_service_find(usbState.vendorID,
                                             usbState.productID, &usbState);
    if (service == 0) {
        ylog("USB device unavailable for restore");
        return false;
    }
    IOUSBDeviceInterface182 **dev = usb_interface_create(service);
    IOObjectRelease(service);
    if (dev == NULL) return false;
    IOReturn result = usb_interface_open(dev);
    if (result != kIOReturnSuccess) {
        ylog("Could not open USB device for restore: 0x%x", result);
        (*dev)->Release(dev);
        return false;
    }
    IOReturn resumed = (*dev)->USBDeviceSuspend(dev, false);
    UInt8 configuration = 0;
    result = (*dev)->GetConfiguration(dev, &configuration);
    if (result == kIOReturnSuccess && configuration == usbState.configuration &&
        resumed == kIOReturnSuccess) {
        usbState.valid = false;
    } else {
        result = (*dev)->SetConfiguration(dev, usbState.configuration);
        if (result == kIOReturnSuccess && resumed == kIOReturnSuccess) {
            UInt8 verified = 0;
            result = (*dev)->GetConfiguration(dev, &verified);
            if (result == kIOReturnSuccess && verified == usbState.configuration) {
                usbState.valid = false;
            }
        }
    }
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    if (usbState.valid) ylog("USB device restore not confirmed: 0x%x", result);
    return !usbState.valid;
}

static Boolean usb_device_disable(int vendorID, int productID) {
    io_service_t service = usb_service_find(vendorID, productID,
        (usbState.valid && usbState.vendorID == vendorID &&
         usbState.productID == productID) ? &usbState : NULL);
    if (service == 0) {
        ylog("USB device unavailable for disable");
        return false;
    }
    uint64_t registryID = 0;
    IORegistryEntryGetRegistryEntryID(service, &registryID);
    uint32_t locationID = (uint32_t)usb_property_number(service,
        CFSTR(kUSBDevicePropertyLocationID));
    IOUSBDeviceInterface182 **dev = usb_interface_create(service);
    if (dev == NULL) {
        IOObjectRelease(service);
        return false;
    }
    if (usbState.valid && usbState.vendorID == vendorID &&
        usbState.productID == productID) {
        UInt8 currentConfiguration = 0;
        IOReturn currentResult = (*dev)->GetConfiguration(dev,
                                                           &currentConfiguration);
        if (currentResult == kIOReturnSuccess && currentConfiguration == 0) {
            (*dev)->Release(dev);
            IOObjectRelease(service);
            return true;
        }
    }
    IOReturn result = usb_interface_open(dev);
    if (result != kIOReturnSuccess) {
        ylog("Could not open USB device for disable: 0x%x", result);
        (*dev)->Release(dev);
        IOObjectRelease(service);
        return false;
    }
    UInt8 configuration = 0;
    result = (*dev)->GetConfiguration(dev, &configuration);
    Boolean success = false;
    if (result != kIOReturnSuccess) {
        ylog("Could not read USB configuration: 0x%x", result);
    } else if (configuration == 0) {
        // An unconfigured device without our saved state cannot be restored.
        success = usbState.valid;
    } else {
        result = (*dev)->SetConfiguration(dev, 0);
        if (result == kIOReturnSuccess) {
            usbState = (USBDeviceState){vendorID, productID, registryID,
                                        locationID, configuration, true};
            IOReturn suspended = (*dev)->USBDeviceSuspend(dev, true);
            if (suspended != kIOReturnSuccess) {
                ylog("USB suspend failed after deconfiguration: 0x%x", suspended);
            }
            success = true;
        } else {
            ylog("Could not deconfigure USB device: 0x%x", result);
        }
    }
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    if (success) {
        IOReturn wakeResult = IORegistryEntrySetCFProperty(service,
            CFSTR("kUSBHostDevicePropertyRemoteWakeOverride"), kCFBooleanFalse);
        if (wakeResult != kIOReturnSuccess) {
            ylog("Could not set remote wake override: 0x%x", wakeResult);
        }
    }
    IOObjectRelease(service);
    return success;
}

static void handle_removal_callback(void *context, IOReturn result,
                                    void *sender, IOHIDDeviceRef device) {
    if (hidDevice == device) {
        ylog( "device unplugged");
        IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
        hidDevice = NULL;
    }
}

static void match_callback(void *context, IOReturn result, void *sender,
                           IOHIDDeviceRef device) {
    if (!desiredDisabled || sender != hidManager) return;
    IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
    if (r == kIOReturnSuccess) {
        ylog( "Open'ed HID device");
        hidDevice = device;
    } else {
        ylog( "Failed to open HID device, error: %d", r);
    }
}

static void close_hid_manager(void) {
    if (hidDevice != NULL) {
        IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
        hidDevice = NULL;
    }
    if (hidManager != NULL) {
        IOHIDManagerUnscheduleFromRunLoop(hidManager, CFRunLoopGetMain(),
                                          kCFRunLoopCommonModes);
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        CFRelease(hidManager);
        hidManager = NULL;
    }
}

static CFDictionaryRef matching_dictionary_create(int vendorID, int productID,
                                                  int usagePage, int usage) {
    CFMutableDictionaryRef match =
        CFDictionaryCreateMutable(kCFAllocatorDefault,
                                  0,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);

    if (vendorID) {
        match_set(match, CFSTR(kIOHIDVendorIDKey), vendorID);
    }
    if (productID) {
        match_set(match, CFSTR(kIOHIDProductIDKey), productID);
    }
    if (usagePage) {
        match_set(match, CFSTR(kIOHIDDeviceUsagePageKey), usagePage);
    }
    if (usage) {
        match_set(match, CFSTR(kIOHIDDeviceUsageKey), usage);
    }

    return match;
}

static Boolean configure_hid_manager(int vendorID, int productID) {
    if (hidManager == NULL) {
        hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (hidManager == NULL) return false;
        IOHIDManagerRegisterDeviceMatchingCallback(hidManager,
                                                   match_callback, NULL);
        IOHIDManagerRegisterDeviceRemovalCallback(hidManager,
                                                  handle_removal_callback, NULL);
        IOHIDManagerScheduleWithRunLoop(hidManager, CFRunLoopGetMain(),
                                        kCFRunLoopCommonModes);
        CFDictionaryRef match = matching_dictionary_create(vendorID, productID,
                                                            1, 6);
        IOHIDManagerSetDeviceMatching(hidManager, match);
        CFRelease(match);
        IOReturn result = IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone);
        if (result != kIOReturnSuccess) {
            ylog("Could not open HID manager: 0x%x", result);
            close_hid_manager();
            return false;
        }
    }
    return true;
}

static void schedule_usb_reconcile(unsigned attempts, uint64_t generation) {
    if (attempts == 0) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (!desiredDisabled || generation != requestGeneration) return;
        if (!usb_device_disable(selectedVendorID, selectedProductID)) {
            schedule_usb_reconcile(attempts - 1, generation);
        }
    });
}

static void usb_matched_callback(void *context, io_iterator_t iterator) {
    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != 0) IOObjectRelease(service);
    if (desiredDisabled) schedule_usb_reconcile(8, requestGeneration);
}

static void __XPC_Peer_Event_Handler(xpc_connection_t connection,
                                     xpc_object_t event) {
    xpc_type_t type = xpc_get_type(event);

    if (type == XPC_TYPE_ERROR) {
        const char *description = xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);
        ylog( "XPC error: %s", description);
        return;
    }
    if (type != XPC_TYPE_DICTIONARY) return;

    int64_t vendor = xpc_dictionary_get_int64(event, "idVendor");
    int64_t product = xpc_dictionary_get_int64(event, "idProduct");
    int64_t action = xpc_dictionary_get_int64(event, "request");
    Boolean valid = (action == 0 || action == 1) &&
                    vendor > 0 && vendor <= UINT16_MAX &&
                    product > 0 && product <= UINT16_MAX;
    Boolean success = false;
    if (valid && action == 1) {
        requestGeneration++;
        success = usbState.valid ? usb_device_enable() :
            usb_device_configured((int)vendor, (int)product);
        if (success) {
            desiredDisabled = false;
            close_hid_manager();
        } else if (desiredDisabled) {
            if (!usb_device_disable(selectedVendorID, selectedProductID)) {
                ylog("Could not reapply disabled state after failed enable");
            }
            schedule_usb_reconcile(8, requestGeneration);
        }
    } else if (valid) {
        int oldVendor = selectedVendorID;
        int oldProduct = selectedProductID;
        Boolean filterChanged = desiredDisabled &&
            (oldVendor != vendor || oldProduct != product);
        if (filterChanged) {
            success = usb_device_enable();
            if (success) close_hid_manager();
        } else {
            success = true;
        }
        if (success) {
            selectedVendorID = (int)vendor;
            selectedProductID = (int)product;
            desiredDisabled = true;
            requestGeneration++;
            Boolean hidReady = configure_hid_manager(selectedVendorID,
                                                      selectedProductID);
            Boolean usbReady = usb_device_disable(selectedVendorID,
                                                   selectedProductID);
            success = hidReady && usbReady;
            if (!success && filterChanged) {
                // Keep the prior key disabled if the new filter fails.
                close_hid_manager();
                selectedVendorID = oldVendor;
                selectedProductID = oldProduct;
                requestGeneration++;
                Boolean oldHidReady = configure_hid_manager(oldVendor,
                                                              oldProduct);
                Boolean oldUsbReady = usb_device_disable(oldVendor,
                                                           oldProduct);
                if (!oldHidReady || !oldUsbReady) {
                    ylog("Could not reapply previous disabled filter");
                }
            }
            if (!usbReady || !success) {
                schedule_usb_reconcile(8, requestGeneration);
            }
        }
    }
    xpc_object_t reply = xpc_dictionary_create_reply(event);
    if (reply != NULL) {
        xpc_dictionary_set_string(reply, "reply", success ? "OK" : "ERROR");
        xpc_connection_t remote = xpc_dictionary_get_remote_connection(event);
        xpc_connection_send_message(remote, reply);
        xpc_release(reply);
    }
}

static void __XPC_Connection_Handler(xpc_connection_t connection) {
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        __XPC_Peer_Event_Handler(connection, event);
    });

    xpc_connection_resume(connection);
}

static void powerCallback(void *refCon, io_service_t service,
                          natural_t messageType, void *messageArgument) {
    switch (messageType) {
        case kIOMessageSystemWillSleep:
            IOAllowPowerChange(pmRootPort, (long)messageArgument);
            break;
        case kIOMessageCanSystemSleep:
            IOAllowPowerChange(pmRootPort, (long)messageArgument);
            break;
        case kIOMessageSystemHasPoweredOn:
            if (desiredDisabled) schedule_usb_reconcile(8, requestGeneration);
            break;
        default:
            break;
    }
}

int main(int argc, const char *argv[]) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
                                                     SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_t interrupt = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
                                                          SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(term, ^{
        Boolean restored = usb_device_enable();
        close_hid_manager();
        if (!restored) ylog("Helper exited before USB restore completed");
        exit(restored ? EXIT_SUCCESS : EXIT_FAILURE);
    });
    dispatch_source_set_event_handler(interrupt, ^{
        Boolean restored = usb_device_enable();
        close_hid_manager();
        if (!restored) ylog("Helper exited before USB restore completed");
        exit(restored ? EXIT_SUCCESS : EXIT_FAILURE);
    });
    dispatch_resume(term);
    dispatch_resume(interrupt);

    // Register for system sleep/wake notifications
    pmRootPort = IORegisterForSystemPower(NULL, &pmNotifyPort,
                                          powerCallback, &pmNotifier);
    if (pmRootPort) {
        CFRunLoopAddSource(CFRunLoopGetMain(),
                           IONotificationPortGetRunLoopSource(pmNotifyPort),
                           kCFRunLoopCommonModes);
        ylog("Registered for system power notifications");
    }

    usbNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (usbNotifyPort != NULL) {
        CFRunLoopAddSource(CFRunLoopGetMain(),
                           IONotificationPortGetRunLoopSource(usbNotifyPort),
                           kCFRunLoopCommonModes);
        kern_return_t result = IOServiceAddMatchingNotification(
            usbNotifyPort, kIOFirstMatchNotification,
            IOServiceMatching("IOUSBHostDevice"), usb_matched_callback,
            NULL, &usbAddedIterator);
        if (result == KERN_SUCCESS) {
            usb_matched_callback(NULL, usbAddedIterator);
        } else {
            ylog("Could not register USB match notification: 0x%x", result);
        }
    }

    xpc_connection_t service = xpc_connection_create_mach_service("com.zgilburd.yubiswitch.helper",
                                                                  dispatch_get_main_queue(),
                                                                  XPC_CONNECTION_MACH_SERVICE_LISTENER);

    if (!service) {
        ylog( "Failed to create service.");
        exit(EXIT_FAILURE);
    }

    ylog( "Configuring connection event handler for helper");
    xpc_connection_set_event_handler(service, ^(xpc_object_t connection) {
        __XPC_Connection_Handler(connection);
    });

    xpc_connection_resume(service);
    CFRunLoopRun();
    dispatch_main();
    return EXIT_SUCCESS;
}
