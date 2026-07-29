# AC Controller API Contract v0.1

Firmware base URL example:

```text
http://192.168.1.10:3000/api/v1
```

Every authenticated request includes:

```http
Authorization: Bearer <deviceToken>
X-Device-Id: ACIR-XXXXXXXXXXXX
Content-Type: application/json
```

## 1. Register device

```http
POST /devices/register
X-Device-Bootstrap-Key: <bootstrap key>
```

Request:

```json
{
  "deviceId": "ACIR-A1B2C3D4E5F6",
  "firmwareVersion": "0.1.0",
  "chipModel": "ESP32-D0WDQ6",
  "chipRevision": 1,
  "mac": "AA:BB:CC:DD:EE:FF",
  "irRxPin": 27,
  "irTxPin": 4
}
```

Response `200` or `201`:

```json
{
  "deviceToken": "device-token-value",
  "pairingCode": "583921",
  "paired": false
}
```

## 2. Heartbeat

```http
POST /devices/{deviceId}/heartbeat
```

Request:

```json
{
  "firmwareVersion": "0.1.0",
  "ip": "192.168.1.105",
  "ssid": "HomeWiFi",
  "rssi": -52,
  "uptimeMs": 45000,
  "freeHeap": 215000,
  "learning": false,
  "activeLearningProfileId": ""
}
```

Response: any `2xx`.

## 3. Poll next command

```http
GET /devices/{deviceId}/commands/next?after={lastCommandId}
```

No command:

```http
204 No Content
```

### Start learning

```json
{
  "command": {
    "id": "cmd-001",
    "type": "START_LEARNING",
    "profileId": "profile-001",
    "expectedAction": "POWER_ON_COOL_26_AUTO",
    "timeoutSeconds": 45
  }
}
```

### Cancel learning

```json
{
  "command": {
    "id": "cmd-002",
    "type": "CANCEL_LEARNING"
  }
}
```

### Send native A/C state

```json
{
  "command": {
    "id": "cmd-003",
    "type": "SET_AC_STATE",
    "profileId": "profile-001",
    "protocol": "ELECTRA_AC",
    "model": -1,
    "power": true,
    "mode": "cool",
    "temperature": 26,
    "celsius": true,
    "fan": "auto",
    "swingV": "off",
    "swingH": "off",
    "quiet": false,
    "turbo": false,
    "econo": false,
    "light": false,
    "filter": false,
    "clean": false,
    "beep": false,
    "sleep": -1,
    "clock": -1
  }
}
```

Suggested string values:

- `mode`: `auto`, `cool`, `heat`, `dry`, `fan`, `off`
- `fan`: `auto`, `min`, `low`, `medium`, `high`, `max`
- `swingV`: `off`, `auto`, `highest`, `high`, `middle`, `low`, `lowest`
- `swingH`: `off`, `auto`, `leftmax`, `left`, `middle`, `right`, `rightmax`, `wide`

### Send raw IR

```json
{
  "command": {
    "id": "cmd-004",
    "type": "SEND_RAW",
    "profileId": "profile-002",
    "frequencyKhz": 38,
    "rawUs": [9008, 4532, 522, 1726, 520, 590]
  }
}
```

### Reset Wi-Fi / factory reset

```json
{"command":{"id":"cmd-005","type":"RESET_WIFI"}}
```

```json
{"command":{"id":"cmd-006","type":"FACTORY_RESET"}}
```

## 4. Command acknowledgement

```http
POST /devices/{deviceId}/commands/{commandId}/ack
```

Request:

```json
{
  "status": "accepted",
  "message": "Waiting for original remote",
  "deviceTimeMs": 123456
}
```

`status` is one of:

- `accepted`
- `completed`
- `failed`

## 5. Upload learned IR signal

```http
POST /devices/{deviceId}/profiles/{profileId}/learned-signals
```

Representative request:

```json
{
  "event": "IR_SIGNAL_LEARNED",
  "deviceId": "ACIR-A1B2C3D4E5F6",
  "commandId": "cmd-001",
  "profileId": "profile-001",
  "expectedAction": "POWER_ON_COOL_26_AUTO",
  "protocol": "ELECTRA_AC",
  "protocolId": 98,
  "bits": 104,
  "code": "0xC377E0006000200000200001BB",
  "stateHex": "C377E0006000200000200001BB",
  "description": "Power: On, Mode: Cool, Temp: 26C...",
  "nativeSendSupported": true,
  "commonDecoded": true,
  "controlType": "NATIVE",
  "commonState": {
    "protocol": "ELECTRA_AC",
    "model": -1,
    "power": true,
    "mode": "Cool",
    "temperature": 26,
    "celsius": true,
    "fan": "Auto",
    "swingV": "Off",
    "swingH": "Off"
  },
  "rawUs": [9008, 4532, 522, 1726],
  "rawCount": 211,
  "rawTruncated": false
}
```

Server should save the **protocol string**, not rely on `protocolId`, because enum numbers are implementation details of the installed IR library version.

## 6. Generic event

```http
POST /devices/{deviceId}/events
```

Learning timeout example:

```json
{
  "event": "IR_LEARNING_TIMEOUT",
  "profileId": "profile-001"
}
```
