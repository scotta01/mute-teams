#pragma once

/*
 * BLE HID-over-GATT service, vendored from the Flipper firmware
 * (lib/ble_profile/extra_services/hid_service.c). The firmware exposes this
 * service only to bundled apps, not to external FAPs, so a custom FAP must
 * carry its own copy and build it on the generic, FAP-exposed GATT API
 * (ble_gatt_service_add / ble_gatt_characteristic_init / ..._update).
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Standard Bluetooth SIG 16-bit UUIDs for the HID-over-GATT service.
 * Defined here because the public Flipper SDK does not ship the ST copro
 * UUID headers (<ble/ble.h>) that normally provide them.
 */
#define HUMAN_INTERFACE_DEVICE_SERVICE_UUID (0x1812)
#define PROTOCOL_MODE_CHAR_UUID             (0x2A4E)
#define REPORT_CHAR_UUID                    (0x2A4D)
#define REPORT_MAP_CHAR_UUID                (0x2A4B)
#define HID_INFORMATION_CHAR_UUID           (0x2A4A)
#define HID_CONTROL_POINT_CHAR_UUID         (0x2A4C)
#define REPORT_REFERENCE_DESCRIPTOR_UUID    (0x2908)

typedef struct BleServiceHid BleServiceHid;

BleServiceHid* ble_svc_hid_start(void);

void ble_svc_hid_stop(BleServiceHid* service);

bool ble_svc_hid_update_report_map(BleServiceHid* service, const uint8_t* data, uint16_t len);

bool ble_svc_hid_update_input_report(
    BleServiceHid* service,
    uint8_t input_report_num,
    uint8_t* data,
    uint16_t len);

// Expects data to be of length BLE_SVC_HID_INFO_LEN (4 bytes)
bool ble_svc_hid_update_info(BleServiceHid* service, uint8_t* data);

#ifdef __cplusplus
}
#endif
