import os
import sys
import time
from flask import Flask, request, jsonify, send_from_directory
from flask_cors import CORS
from storage import storage

BOOTSTRAP_KEY = os.getenv("DEVICE_BOOTSTRAP_KEY", "CHANGE_ME_BOOTSTRAP_KEY")
WEB_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "web"))

app = Flask(__name__, static_folder=WEB_DIR)
CORS(app)

# ============================================================
# AUTH HELPERS
# ============================================================

def get_current_user():
    """Lấy user từ session token trong header hoặc cookie."""
    auth = request.headers.get("Authorization", "")
    token = auth.replace("Bearer ", "").strip()
    if not token:
        token = request.cookies.get("session_token", "")
    if not token:
        return None
    return storage.verify_session(token)

def require_user():
    user = get_current_user()
    if not user:
        return None, jsonify({"error": "Chưa đăng nhập"}), 401
    return user, None, None

def authenticate_device(device_id):
    auth_header = request.headers.get("Authorization", "")
    token = auth_header.replace("Bearer ", "").strip()
    if not token or not storage.verify_token(device_id, token):
        return False
    return True

# ============================================================
# USER AUTH ROUTES
# ============================================================

@app.route("/api/v1/auth/register", methods=["POST"])
def auth_register():
    data = request.get_json(silent=True) or {}
    username     = data.get("username", "").strip()
    password     = data.get("password", "").strip()
    display_name = data.get("displayName", "").strip()

    result, error = storage.register_user(username, password, display_name)
    if error:
        return jsonify({"error": error}), 400
    return jsonify({"success": True, "user": result}), 201

@app.route("/api/v1/auth/login", methods=["POST"])
def auth_login():
    data = request.get_json(silent=True) or {}
    username = data.get("username", "").strip()
    password = data.get("password", "").strip()

    result, error = storage.login_user(username, password)
    if error:
        return jsonify({"error": error}), 401
    return jsonify({"success": True, "user": result}), 200

@app.route("/api/v1/auth/logout", methods=["POST"])
def auth_logout():
    auth = request.headers.get("Authorization", "")
    token = auth.replace("Bearer ", "").strip()
    if token:
        storage.logout_user(token)
    return jsonify({"success": True}), 200

@app.route("/api/v1/auth/me", methods=["GET"])
def auth_me():
    user = get_current_user()
    if not user:
        return jsonify({"error": "Chưa đăng nhập"}), 401
    return jsonify({
        "userId":      user["userId"],
        "username":    user["username"],
        "displayName": user["displayName"],
        "deviceCount": len(user.get("deviceIds", []))
    })

# ============================================================
# FIRMWARE API (CONTRACT v0.1)
# ============================================================

@app.route("/api/v1/devices/register", methods=["POST"])
def register_device():
    data      = request.get_json(silent=True) or {}
    device_id = data.get("deviceId") or request.headers.get("X-Device-Id")
    if not device_id:
        return jsonify({"error": "Missing deviceId"}), 400

    token, code = storage.register_device(data)
    print(f"[Device Registered] ID: {device_id} | Pairing Code: {code}")
    return jsonify({"deviceToken": token, "pairingCode": code, "paired": False}), 201

@app.route("/api/v1/devices/<device_id>/heartbeat", methods=["POST"])
def device_heartbeat(device_id):
    if not authenticate_device(device_id):
        return jsonify({"error": "Unauthorized"}), 401
    data = request.get_json(silent=True) or {}
    storage.update_heartbeat(device_id, data)
    return jsonify({"status": "ok", "serverTime": time.time()}), 200

@app.route("/api/v1/devices/<device_id>/commands/next", methods=["GET"])
def get_next_command(device_id):
    if not authenticate_device(device_id):
        return jsonify({"error": "Unauthorized"}), 401
    after_id = request.args.get("after")
    cmd = storage.get_next_pending_command(device_id, after_id)
    if not cmd:
        return "", 204
    clean_cmd = {"id": cmd["id"], "type": cmd["type"]}
    for k, v in cmd.items():
        if k not in ["deviceId", "status", "createdAt", "ackStatus", "ackMessage", "ackAt"]:
            clean_cmd[k] = v
    return jsonify({"command": clean_cmd}), 200

@app.route("/api/v1/devices/<device_id>/commands/<command_id>/ack", methods=["POST"])
def ack_command(device_id, command_id):
    if not authenticate_device(device_id):
        return jsonify({"error": "Unauthorized"}), 401
    data    = request.get_json(silent=True) or {}
    status  = data.get("status", "completed")
    message = data.get("message", "")
    storage.ack_command(device_id, command_id, status, message)
    print(f"[Command ACK] {device_id} -> Cmd {command_id}: status={status}")
    return jsonify({"status": "ok"}), 200

@app.route("/api/v1/devices/<device_id>/profiles/<profile_id>/learned-signals", methods=["POST"])
def upload_learned_signal(device_id, profile_id):
    if not authenticate_device(device_id):
        return jsonify({"error": "Unauthorized"}), 401
    data = request.get_json(silent=True) or {}
    prof = storage.save_learned_signal(device_id, profile_id, data)
    print(f"[IR Signal Learned] Device: {device_id} | Profile: {profile_id} | Action: {data.get('expectedAction')}")
    return jsonify({"status": "ok", "profile": prof}), 200

@app.route("/api/v1/devices/<device_id>/events", methods=["POST"])
def device_event(device_id):
    if not authenticate_device(device_id):
        return jsonify({"error": "Unauthorized"}), 401
    data       = request.get_json(silent=True) or {}
    event_type = data.get("event")
    profile_id = data.get("profileId")
    if event_type == "IR_LEARNING_TIMEOUT":
        storage.set_learning_timeout(device_id, profile_id)
        print(f"[Device Event] IR_LEARNING_TIMEOUT for profile {profile_id}")
    return jsonify({"status": "ok"}), 200

# ============================================================
# WEB UI API — yêu cầu user session
# ============================================================

@app.route("/api/v1/web/devices", methods=["GET"])
def web_list_devices():
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    user_device_ids = storage.get_user_device_ids(user)
    devices = storage.list_devices(user_device_ids=user_device_ids)
    return jsonify({"devices": devices})

@app.route("/api/v1/web/pair", methods=["POST"])
def web_pair_device():
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code

    data = request.get_json(silent=True) or {}
    code = data.get("code", "").strip()
    if not code:
        return jsonify({"error": "Vui lòng nhập Mã ghép nối hoặc Device ID"}), 400

    device = storage.pair_device(code, user=user)
    if not device:
        return jsonify({"error": "Không tìm thấy thiết bị với mã này"}), 444
    return jsonify({"success": True, "device": device})

@app.route("/api/v1/web/devices/<device_id>/control", methods=["POST"])
def web_control_ac(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code

    # Kiểm tra thiết bị thuộc user
    if device_id not in storage.get_user_device_ids(user):
        return jsonify({"error": "Bạn không có quyền điều khiển thiết bị này"}), 403

    data = request.get_json(silent=True) or {}
    dev  = storage.get_device(device_id)
    if not dev:
        return jsonify({"error": "Thiết bị không tồn tại"}), 404

    cmd_payload = {
        "profileId":    data.get("profileId", "default_profile"),
        "protocol":     data.get("protocol",  "ELECTRA_AC"),
        "model":        int(data.get("model", -1)),
        "power":        bool(data.get("power", True)),
        "mode":         data.get("mode",    "cool"),
        "temperature":  float(data.get("temperature", 26.0)),
        "celsius":      bool(data.get("celsius", True)),
        "fan":          data.get("fan",     "auto"),
        "swingV":       data.get("swingV",  "off"),
        "swingH":       data.get("swingH",  "off"),
        "quiet":        bool(data.get("quiet",  False)),
        "turbo":        bool(data.get("turbo",  False)),
        "econo":        bool(data.get("econo",  False)),
        "light":        bool(data.get("light",  False)),
        "filter":       bool(data.get("filter", False)),
        "clean":        bool(data.get("clean",  False)),
        "beep":         bool(data.get("beep",   False)),
        "sleep":        int(data.get("sleep", -1)),
        "clock":        int(data.get("clock", -1))
    }

    cmd = storage.add_command(device_id, "SET_AC_STATE", cmd_payload)
    print(f"[Web Control AC] Queued command {cmd['id']} for device {device_id}")
    return jsonify({"success": True, "command": cmd})

@app.route("/api/v1/web/devices/<device_id>/learn", methods=["POST"])
def web_start_learn(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    if device_id not in storage.get_user_device_ids(user):
        return jsonify({"error": "Bạn không có quyền với thiết bị này"}), 403

    data            = request.get_json(silent=True) or {}
    profile_id      = data.get("profileId", f"prof_{int(time.time())}")
    expected_action = data.get("expectedAction", "POWER_ON_COOL_26")
    timeout_seconds = int(data.get("timeoutSeconds", 45))

    cmd_payload = {"profileId": profile_id, "expectedAction": expected_action,
                   "timeoutSeconds": timeout_seconds}
    cmd = storage.add_command(device_id, "START_LEARNING", cmd_payload)

    with storage.lock:
        if device_id in storage.devices:
            storage.devices[device_id]["learning"] = True
            storage.devices[device_id]["activeLearningProfileId"] = profile_id
            storage._save_json(storage.devices_file, storage.devices)

    return jsonify({"success": True, "command": cmd, "profileId": profile_id})

@app.route("/api/v1/web/devices/<device_id>/cancel-learn", methods=["POST"])
def web_cancel_learn(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    cmd = storage.add_command(device_id, "CANCEL_LEARNING", {})
    storage.set_learning_timeout(device_id, "")
    return jsonify({"success": True, "command": cmd})

@app.route("/api/v1/web/devices/<device_id>/send-raw", methods=["POST"])
def web_send_raw(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    if device_id not in storage.get_user_device_ids(user):
        return jsonify({"error": "Bạn không có quyền với thiết bị này"}), 403

    data          = request.get_json(silent=True) or {}
    raw_us        = data.get("rawUs", [])
    frequency_khz = int(data.get("frequencyKhz", 38))
    profile_id    = data.get("profileId", "raw_profile")

    if not raw_us:
        return jsonify({"error": "Dữ liệu rawUs trống"}), 400

    cmd_payload = {"profileId": profile_id, "frequencyKhz": frequency_khz, "rawUs": raw_us}
    cmd = storage.add_command(device_id, "SEND_RAW", cmd_payload)
    return jsonify({"success": True, "command": cmd})

@app.route("/api/v1/web/devices/<device_id>/profiles", methods=["GET"])
def web_get_profiles(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    profiles = storage.get_profiles(device_id)
    return jsonify({"profiles": profiles})

@app.route("/api/v1/web/devices/<device_id>/profiles/create", methods=["POST"])
def web_create_profile(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    if device_id not in storage.get_user_device_ids(user):
        return jsonify({"error": "Bạn không có quyền với thiết bị này"}), 403

    data    = request.get_json(silent=True) or {}
    name    = data.get("name", "").strip()
    profile = storage.create_profile(device_id, name)
    return jsonify({"success": True, "profile": profile})

@app.route("/api/v1/web/profiles/<profile_id>", methods=["DELETE"])
def web_delete_profile(profile_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    ok = storage.delete_profile(profile_id)
    if ok:
        return jsonify({"success": True})
    return jsonify({"error": "Profile not found"}), 404

@app.route("/api/v1/web/commands/<command_id>/status", methods=["GET"])
def web_command_status(command_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    cmd = storage.get_command_status(command_id)
    if not cmd:
        return jsonify({"error": "Command not found"}), 404
    return jsonify({"command": cmd})

# ============================================================
# Static Web UI
# ============================================================
@app.route("/")
def index():
    return send_from_directory(WEB_DIR, "index.html")

@app.route("/<path:filename>")
def serve_static(filename):
    return send_from_directory(WEB_DIR, filename)

# ============================================================
if __name__ == "__main__":
    host = os.getenv("HOST", "0.0.0.0")
    port = int(os.getenv("PORT", 3000))
    print(f"==================================================")
    print(f"  AC CONTROLLER BACKEND SERVER RUNNING")
    print(f"  API Endpoint: http://{host}:{port}/api/v1")
    print(f"  Web Dashboard: http://localhost:{port}")
    print(f"==================================================")
    app.run(host=host, port=port, debug=True)
