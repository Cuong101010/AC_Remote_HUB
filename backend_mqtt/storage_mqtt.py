import os
import json
import time
import uuid
import hashlib
import threading

if "VERCEL" in os.environ:
    DATA_DIR = "/tmp/data"
else:
    DATA_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "backend", "data"))

class StorageMqtt:
    def __init__(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        self.devices_file  = os.path.join(DATA_DIR, "devices.json")
        self.commands_file = os.path.join(DATA_DIR, "commands.json")
        self.profiles_file = os.path.join(DATA_DIR, "profiles.json")
        self.users_file    = os.path.join(DATA_DIR, "users.json")
        self.sessions_file = os.path.join(DATA_DIR, "sessions.json")
        self.lock = threading.Lock()

        self.devices  = self._load_json(self.devices_file,  {})
        self.commands = self._load_json(self.commands_file, [])
        self.profiles = self._load_json(self.profiles_file, {})
        self.users    = self._load_json(self.users_file,    {})
        self.sessions = self._load_json(self.sessions_file, {})
        self.sensor_data: dict = {}

    def _load_json(self, filepath, default):
        if os.path.exists(filepath):
            try:
                with open(filepath, "r", encoding="utf-8") as f:
                    return json.load(f)
            except Exception as e:
                print(f"[StorageMQTT] Error reading {filepath}: {e}")
        return default

    def _save_json(self, filepath, data):
        try:
            temp_file = filepath + ".tmp"
            with open(temp_file, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
            os.replace(temp_file, filepath)
        except Exception as e:
            print(f"[StorageMQTT] Error writing {filepath}: {e}")

    def _hash_password(self, password):
        return hashlib.sha256(password.encode("utf-8")).hexdigest()

    def register_user(self, username, password, display_name=""):
        with self.lock:
            username = username.strip().lower()
            if not username or not password:
                return None, "Tên đăng nhập và mật khẩu không được để trống"
            if len(username) < 3:
                return None, "Tên đăng nhập tối thiểu 3 ký tự"
            if len(password) < 6:
                return None, "Mật khẩu tối thiểu 6 ký tự"
            if username in self.users:
                return None, "Tên đăng nhập đã tồn tại"

            user_id = f"usr_{uuid.uuid4().hex[:12]}"
            user = {
                "userId": user_id,
                "username": username,
                "displayName": display_name.strip() or username,
                "passwordHash": self._hash_password(password),
                "createdAt": time.time(),
                "deviceIds": []
            }
            self.users[username] = user
            self._save_json(self.users_file, self.users)

            session_token = self._create_session_unlocked(user_id)
            return {"userId": user_id, "username": username,
                    "displayName": user["displayName"],
                    "sessionToken": session_token}, None

    def login_user(self, username, password):
        with self.lock:
            username = username.strip().lower()
            user = self.users.get(username)
            if not user:
                return None, "Tài khoản không tồn tại"
            if user["passwordHash"] != self._hash_password(password):
                return None, "Mật khẩu không đúng"

            session_token = self._create_session_unlocked(user["userId"])
            return {"userId": user["userId"], "username": username,
                    "displayName": user["displayName"],
                    "sessionToken": session_token}, None

    def _create_session_unlocked(self, user_id):
        token = f"sess_{uuid.uuid4().hex}"
        self.sessions[token] = {
            "userId": user_id,
            "createdAt": time.time(),
            "expiresAt": time.time() + 30 * 24 * 3600
        }
        self._save_json(self.sessions_file, self.sessions)
        return token

    def verify_session(self, session_token):
        with self.lock:
            sess = self.sessions.get(session_token)
            if not sess:
                return None
            if time.time() > sess["expiresAt"]:
                del self.sessions[session_token]
                self._save_json(self.sessions_file, self.sessions)
                return None
            user_id = sess["userId"]
            for u in self.users.values():
                if u["userId"] == user_id:
                    return u
            return None

    def logout_user(self, session_token):
        with self.lock:
            if session_token in self.sessions:
                del self.sessions[session_token]
                self._save_json(self.sessions_file, self.sessions)

    def assign_device_to_user(self, username, device_id):
        with self.lock:
            user = self.users.get(username)
            if not user:
                return False
            if device_id not in user.get("deviceIds", []):
                user.setdefault("deviceIds", []).append(device_id)
                self._save_json(self.users_file, self.users)
            return True

    def get_user_device_ids(self, user):
        return user.get("deviceIds", [])

    def register_device(self, reg_data):
        with self.lock:
            device_id = reg_data.get("deviceId")
            if not device_id:
                return None, None

            existing = self.devices.get(device_id, {})
            device_token  = existing.get("deviceToken")  or f"tok_{uuid.uuid4().hex}"
            pairing_code  = existing.get("pairingCode")  or str(uuid.uuid4().int)[:6].zfill(6)

            device_info = {
                "deviceId":        device_id,
                "deviceToken":     device_token,
                "pairingCode":     pairing_code,
                "paired":          existing.get("paired", False),
                "ownedByUserId":   existing.get("ownedByUserId", ""),
                "firmwareVersion": reg_data.get("firmwareVersion", "0.3.0-MQTT"),
                "chipModel":       reg_data.get("chipModel",  "ESP32"),
                "chipRevision":    reg_data.get("chipRevision", 1),
                "mac":             reg_data.get("mac",   ""),
                "irRxPin":         reg_data.get("irRxPin", 27),
                "irTxPin":         reg_data.get("irTxPin",  4),
                "registeredAt":    existing.get("registeredAt", time.time()),
                "lastSeen":        time.time(),
                "online":          True,
                "status":          "online",
                "ip":              existing.get("ip",       ""),
                "ssid":            existing.get("ssid",     ""),
                "rssi":            existing.get("rssi",      0),
                "freeHeap":        existing.get("freeHeap",  0),
                "uptimeMs":        existing.get("uptimeMs",  0),
                "mqttBroker":      "broker.hivemq.com"
            }

            self.devices[device_id] = device_info
            self._save_json(self.devices_file, self.devices)
            return device_token, pairing_code

    def update_heartbeat(self, device_id, hb_data):
        with self.lock:
            if device_id not in self.devices:
                return False
            dev = self.devices[device_id]
            dev["firmwareVersion"] = hb_data.get("firmwareVersion", dev.get("firmwareVersion"))
            dev["ip"]              = hb_data.get("ip",    dev.get("ip"))
            dev["rssi"]            = hb_data.get("rssi",  dev.get("rssi"))
            dev["uptimeMs"]        = hb_data.get("uptimeMs",  dev.get("uptimeMs"))
            dev["lastSeen"]        = time.time()
            dev["online"]          = True
            dev["status"]          = "online"
            self._save_json(self.devices_file, self.devices)
            return True

    def get_device(self, device_id):
        with self.lock:
            dev = self.devices.get(device_id)
            if dev:
                dev_copy = dict(dev)
                if time.time() - dev_copy.get("lastSeen", 0) > 45:
                    dev_copy["online"] = False
                    dev_copy["status"] = "offline"
                return dev_copy
            return None

    def list_devices(self, user_device_ids=None):
        with self.lock:
            result = []
            now = time.time()
            for dev in self.devices.values():
                if user_device_ids is not None and dev["deviceId"] not in user_device_ids:
                    continue
                dev_copy = dict(dev)
                if now - dev_copy.get("lastSeen", 0) > 45:
                    dev_copy["online"] = False
                    dev_copy["status"] = "offline"
                result.append(dev_copy)
            return result

    def verify_token(self, device_id, token):
        with self.lock:
            dev = self.devices.get(device_id)
            if dev and dev.get("deviceToken") == token:
                return True
            return False

    def pair_device(self, device_id_or_code, user=None):
        with self.lock:
            for dev_id, dev in self.devices.items():
                if dev_id == device_id_or_code or dev.get("pairingCode") == device_id_or_code:
                    dev["paired"] = True
                    if user:
                        dev["ownedByUserId"] = user["userId"]
                        username = user["username"]
                        u = self.users.get(username)
                        if u and dev_id not in u.get("deviceIds", []):
                            u.setdefault("deviceIds", []).append(dev_id)
                            self._save_json(self.users_file, self.users)
                    self._save_json(self.devices_file, self.devices)
                    return dict(dev)
            return None

    def add_command(self, device_id, cmd_type, cmd_payload):
        with self.lock:
            cmd_id = f"cmd_{int(time.time()*1000)}_{uuid.uuid4().hex[:4]}"
            command = {
                "id": cmd_id,
                "deviceId": device_id,
                "type": cmd_type,
                "status": "pending",
                "createdAt": time.time(),
                "ackStatus": None,
                "ackMessage": None
            }
            command.update(cmd_payload)
            self.commands.append(command)
            if len(self.commands) > 500:
                self.commands = self.commands[-500:]
            self._save_json(self.commands_file, self.commands)
            return command

    def get_next_pending_command(self, device_id, after_id=None):
        with self.lock:
            if after_id:
                after_exists = any(cmd.get("id") == after_id for cmd in self.commands if cmd.get("deviceId") == device_id)
                if not after_exists:
                    after_id = None

            found_after = False if after_id else True
            for cmd in self.commands:
                if cmd.get("deviceId") != device_id:
                    continue
                if after_id and not found_after:
                    if cmd.get("id") == after_id:
                        found_after = True
                    continue
                if cmd.get("status") == "pending":
                    cmd["status"] = "sent"
                    self._save_json(self.commands_file, self.commands)
                    return dict(cmd)
            return None

    def ack_command(self, device_id, command_id, status, message):
        with self.lock:
            for cmd in self.commands:
                if cmd.get("id") == command_id and cmd.get("deviceId") == device_id:
                    cmd["status"]     = status
                    cmd["ackStatus"]  = status
                    cmd["ackMessage"] = message
                    cmd["ackAt"]      = time.time()
                    self._save_json(self.commands_file, self.commands)
                    return dict(cmd)
            return None

    def get_command_status(self, command_id):
        with self.lock:
            for cmd in self.commands:
                if cmd.get("id") == command_id:
                    return dict(cmd)
            return None

    def create_profile(self, device_id, name=""):
        with self.lock:
            count = len([p for p in self.profiles.values() if p.get("deviceId") == device_id]) + 1
            profile_id = f"ac_prof_{int(time.time())}"
            prof_name  = name.strip() if name.strip() else f"Điều Hòa #{count}"

            profile_data = {
                "profileId":   profile_id,
                "name":        prof_name,
                "deviceId":    device_id,
                "createdAt":   time.time(),
                "updatedAt":   time.time(),
                "protocol":    "",
                "controlType": "NATIVE",
                "signals":     []
            }
            self.profiles[profile_id] = profile_data
            self._save_json(self.profiles_file, self.profiles)
            return profile_data

    def delete_profile(self, profile_id):
        with self.lock:
            if profile_id in self.profiles:
                del self.profiles[profile_id]
                self._save_json(self.profiles_file, self.profiles)
                return True
            return False

    def delete_learned_signal(self, device_id, profile_id, action_name):
        with self.lock:
            prof = self.profiles.get(profile_id)
            if not prof or prof.get("deviceId") != device_id:
                return False
            signals = prof.get("signals", [])
            initial_len = len(signals)
            new_signals = [s for s in signals if s.get("expectedAction") != action_name and s.get("action") != action_name]
            if len(new_signals) < initial_len:
                prof["signals"] = new_signals
                prof["updatedAt"] = time.time()
                self._save_json(self.profiles_file, self.profiles)
                return True
            return False

    def save_learned_signal(self, device_id, profile_id, signal_data):
        with self.lock:
            if profile_id not in self.profiles:
                count = len([p for p in self.profiles.values() if p.get("deviceId") == device_id]) + 1
                self.profiles[profile_id] = {
                    "profileId":   profile_id,
                    "name":        f"Điều Hòa #{count}",
                    "deviceId":    device_id,
                    "createdAt":   time.time(),
                    "updatedAt":   time.time(),
                    "protocol":    signal_data.get("protocol") or "",
                    "controlType": signal_data.get("controlType") or "NATIVE",
                    "signals":     []
                }

            prof = self.profiles[profile_id]
            prof["updatedAt"] = time.time()

            protocol = signal_data.get("protocol")
            if protocol and protocol != "UNKNOWN":
                prof["protocol"]    = protocol
                prof["controlType"] = "NATIVE"
            else:
                if not prof.get("controlType"):
                    prof["controlType"] = "RAW"

            action_name = signal_data.get("expectedAction", "ACTION_DEFAULT")
            signals = prof.get("signals", [])
            signals = [s for s in signals if s.get("expectedAction") != action_name]
            signals.append({
                "action":        action_name,
                "expectedAction":action_name,
                "protocol":      signal_data.get("protocol"),
                "bits":          signal_data.get("bits"),
                "code":          signal_data.get("code"),
                "address":       signal_data.get("address", 0),
                "commandCode":   signal_data.get("commandCode", 0),
                "repeatCount":   signal_data.get("repeatCount", 0),
                "stateHex":      signal_data.get("stateHex"),
                "rawUs":         signal_data.get("rawUs", []),
                "rawCount":      signal_data.get("rawCount", 0),
                "timestamp":     time.time()
            })
            prof["signals"] = signals

            if device_id in self.devices:
                self.devices[device_id]["learning"] = False
                self.devices[device_id]["activeLearningProfileId"] = ""
                self._save_json(self.devices_file, self.devices)

            self._save_json(self.profiles_file, self.profiles)
            return prof

    def get_profiles(self, device_id=None):
        with self.lock:
            if not device_id:
                return list(self.profiles.values())
            return [p for p in self.profiles.values() if p.get("deviceId") == device_id]

    def update_sensor_data(self, device_id, data):
        with self.lock:
            entry = self.sensor_data.get(device_id, {})
            if data.get("temperature") is not None:
                entry["temperature"] = data["temperature"]
            if data.get("humidity") is not None:
                entry["humidity"] = data["humidity"]
            entry["updatedAt"] = time.time()
            self.sensor_data[device_id] = entry

    def get_sensor_data(self, device_id):
        with self.lock:
            return dict(self.sensor_data.get(device_id, {}))

storage_mqtt = StorageMqtt()
