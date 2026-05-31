# Parkside Battery — Tuya BLE Protocol Notes

## Device Identity

| Field | Value |
|---|---|
| MAC | `dc:23:4d:eb:88:46` |
| BLE Name | `COMMON` |
| Vendor | Tuya Smart / CMIT (`0x248A`) |
| Product ID | `0x8266` |
| Firmware version | `0x0001` |

## GATT Map

| Service | UUID | Description |
|---|---|---|
| GAP | `0x1800` | Standard |
| GATT | `0x1801` | Standard |
| Device Info | `0x180a` | Standard |
| **Tuya BLE** | **`0x1910`** | **Proprietary** |

Key Tuya characteristics (service `0x1910`):

| Char UUID | Role | Notes |
|---|---|---|
| `0x2b10` | Notify (RX) | Device → ESP32 |
| `0x2b11` | Write (TX) | ESP32 → Device |

GATT MTU = 23 (max payload 20 bytes — matches `GATT_MTU = 20` in ha_tuya_ble).

Connection params: 25–50 ms interval, 10 s supervision timeout.

## Protocol: Tuya BLE v3

This is **not** a BM2-style device (no `0xfff4` notify char, no hardcoded AES key).
It speaks the full Tuya BLE session protocol used by `ha_tuya_ble`.

### Session handshake sequence

All traffic over `0x2b10` / `0x2b11`. Packets are AES-CBC encrypted, fragmented into 20-byte GATT chunks with a varint packet-number header.

```
ESP32                                    Battery
  |--FUN_SENDER_DEVICE_INFO (0x0000)------->|   encrypted with login_key
  |<--device info response-------------------|   contains srand[6], auth_key
  |--FUN_SENDER_PAIR (0x0001)--------------->|   encrypted with session_key
  |<--pair response (0x00 = ok)--------------|
  |--FUN_SENDER_DEVICE_STATUS (0x0003)------>|
  |<--FUN_RECEIVE_DP (0x8001)----------------|   datapoints payload
```

### Key derivation

```
local_key       = device secret from Tuya cloud (16 bytes)
login_key       = MD5(local_key[:6])          # used for FUN_SENDER_DEVICE_INFO
session_key     = MD5(local_key + srand)       # used for everything after
```

### Datapoint wire format (inside decrypted payload)

```
[ dp_id : u8 ][ type : u8 ][ len : u8 ][ value : len bytes ]
```

Types: `0`=RAW, `1`=BOOL, `2`=VALUE (signed big-endian int), `3`=STRING, `4`=ENUM, `5`=BITMAP.

## Observed session behavior

- Device advertises and accepts GATT connection immediately.
- If no `FUN_SENDER_DEVICE_INFO` is sent within ~30 s, device disconnects (session timeout).
- Reading `0x2b11` cold returns 20 zero bytes (no unsolicited data before pairing).

## Blocker

`local_key` is required to compute `login_key` and `session_key`. Without it the
`FUN_SENDER_DEVICE_INFO` packet cannot be encrypted and the device rejects the session.

**How to obtain:** Tuya IoT Platform developer account → link the same Tuya/Smart Life app
account → query device list API → `local_key` field. Tools: `tinytuya`, `tuya-cli`.

## Reference

- Protocol implementation: `../ha_tuya_ble/custom_components/tuya_ble/tuya_ble/tuya_ble.py`
- Constants / UUIDs: `../ha_tuya_ble/custom_components/tuya_ble/tuya_ble/const.py`
- Captured GATT log: `log.txt`
