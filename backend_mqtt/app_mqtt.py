import os
import sys
import time
import json
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from flask import Flask, request, jsonify, send_from_directory
from storage_mqtt import storage_mqtt

try:
    from flask_cors import CORS
    has_cors = True
except ImportError:
    has_cors = False

try:
    import paho.mqtt.publish as publish
    has_mqtt = True
except ImportError:
    has_mqtt = False

MQTT_BROKER_HOST = os.getenv("MQTT_BROKER_HOST", "broker.hivemq.com")
MQTT_BROKER_PORT = int(os.getenv("MQTT_BROKER_PORT", "1883"))
MQTT_BROKER_USER = os.getenv("MQTT_BROKER_USER", "")
MQTT_BROKER_PASS = os.getenv("MQTT_BROKER_PASS", "")

def _mqtt_publish_worker(device_id, cmd):
    if not has_mqtt:
        print("[MQTT Worker] Warning: paho-mqtt module not installed.")
        return
    try:
        topic = f"acremote/devices/{device_id}/commands"
        payload = json.dumps({"command": cmd})
        auth = None
        if MQTT_BROKER_USER:
            auth = {"username": MQTT_BROKER_USER, "password": MQTT_BROKER_PASS}
        publish.single(topic, payload, hostname=MQTT_BROKER_HOST, port=MQTT_BROKER_PORT, auth=auth, qos=1)
        print(f"⚡ [MQTT INSTANT PUSH] Published command {cmd['id']} to topic '{topic}'")
    except Exception as e:
        print(f"[MQTT Push Error] {e}")

def publish_mqtt_command(device_id, cmd):
    """Gửi lệnh tức thì không chặn (Non-blocking background thread) qua MQTT Broker."""
    t = threading.Thread(target=_mqtt_publish_worker, args=(device_id, cmd), daemon=True)
    t.start()

WEB_MQTT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "web_mqtt"))

app = Flask(__name__, static_folder=WEB_MQTT_DIR)
if has_cors:
    CORS(app)

@app.after_request
def add_cors_headers(response):
    response.headers['Access-Control-Allow-Origin'] = '*'
    response.headers['Access-Control-Allow-Headers'] = 'Content-Type,Authorization'
    response.headers['Access-Control-Allow-Methods'] = 'GET,POST,PUT,DELETE,OPTIONS'
    return response

# ============================================================
# AUTH HELPERS
# ============================================================

def get_current_user():
    auth = request.headers.get("Authorization", "")
    token = auth.replace("Bearer ", "").strip()
    if not token:
        token = request.cookies.get("session_token", "")
    if not token:
        return None
    return storage_mqtt.verify_session(token)

def require_user():
    user = get_current_user()
    if not user:
        return None, jsonify({"error": "Chưa đăng nhập"}), 401
    return user, None, None

def authenticate_device(device_id):
    token = request.headers.get("X-Device-Token", "").strip()
    if not token:
        auth_header = request.headers.get("Authorization", "")
        token = auth_header.replace("Bearer ", "").strip()
    if not token or not storage_mqtt.verify_token(device_id, token):
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

    result, error = storage_mqtt.register_user(username, password, display_name)
    if error:
        return jsonify({"error": error}), 400
    return jsonify({"success": True, "user": result}), 201

@app.route("/api/v1/auth/login", methods=["POST"])
def auth_login():
    data = request.get_json(silent=True) or {}
    username = data.get("username", "").strip()
    password = data.get("password", "").strip()

    result, error = storage_mqtt.login_user(username, password)
    if error:
        return jsonify({"error": error}), 401
    return jsonify({"success": True, "user": result}), 200

@app.route("/api/v1/auth/logout", methods=["POST"])
def auth_logout():
    auth = request.headers.get("Authorization", "")
    token = auth.replace("Bearer ", "").strip()
    if token:
        storage_mqtt.logout_user(token)
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
# FIRMWARE MQTT & REST API
# ============================================================

@app.route("/api/v1/devices/register", methods=["POST"])
def register_device():
    data      = request.get_json(silent=True) or {}
    device_id = data.get("deviceId") or request.headers.get("X-Device-Id")
    if not device_id:
        return jsonify({"error": "Missing deviceId"}), 400

    token, code = storage_mqtt.register_device(data)
    print(f"[MQTT Device Registered] ID: {device_id} | Pairing Code: {code}")
    return jsonify({"deviceToken": token, "pairingCode": code, "paired": False, "mqttBroker": MQTT_BROKER_HOST}), 201

@app.route("/api/v1/devices/<device_id>/heartbeat", methods=["POST"])
def device_heartbeat(device_id):
    if not authenticate_device(device_id):
        return jsonify({"error": "Unauthorized"}), 401
    data = request.get_json(silent=True) or {}
    storage_mqtt.update_heartbeat(device_id, data)
    if "temperature" in data or "humidity" in data:
        storage_mqtt.update_sensor_data(device_id, {
            "temperature": data.get("temperature"),
            "humidity":    data.get("humidity")
        })
    return jsonify({"status": "ok", "serverTime": time.time()}), 200

@app.route("/api/v1/devices/<device_id>/commands/next", methods=["GET"])
def get_next_command(device_id):
    if not authenticate_device(device_id):
        return jsonify({"error": "Unauthorized"}), 401
    storage_mqtt.update_heartbeat(device_id, {})
    after_id = request.args.get("after")
    cmd = storage_mqtt.get_next_pending_command(device_id, after_id)
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
    storage_mqtt.ack_command(device_id, command_id, status, message)
    print(f"[MQTT Command ACK] {device_id} -> Cmd {command_id}: status={status}")
    return jsonify({"status": "ok"}), 200

# ============================================================
# WEB UI CONTROL API (MQTT PUSH ENABLED)
# ============================================================

@app.route("/api/v1/web/devices", methods=["GET"])
def web_list_devices():
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    user_device_ids = storage_mqtt.get_user_device_ids(user)
    devices = storage_mqtt.list_devices(user_device_ids=user_device_ids)
    return jsonify({"devices": devices})

@app.route("/api/v1/web/devices/<device_id>/sensor", methods=["GET"])
def web_get_sensor(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    data = storage_mqtt.get_sensor_data(device_id)
    return jsonify({"sensor": data})

@app.route("/api/v1/web/pair", methods=["POST"])
def web_pair_device():
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code

    data = request.get_json(silent=True) or {}
    code = data.get("code", "").strip()
    if not code:
        return jsonify({"error": "Vui lòng nhập Mã ghép nối hoặc Device ID"}), 400

    device = storage_mqtt.pair_device(code, user=user)
    if not device:
        return jsonify({"error": "Không tìm thấy thiết bị với mã này"}), 404
    return jsonify({"success": True, "device": device})

@app.route("/api/v1/web/devices/<device_id>/control", methods=["POST"])
def web_control_ac(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code

    if device_id not in storage_mqtt.get_user_device_ids(user):
        return jsonify({"error": "Bạn không có quyền điều khiển thiết bị này"}), 403

    data = request.get_json(silent=True) or {}
    dev  = storage_mqtt.get_device(device_id)
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

    cmd = storage_mqtt.add_command(device_id, "SET_AC_STATE", cmd_payload)
    publish_mqtt_command(device_id, cmd)
    return jsonify({"success": True, "command": cmd, "instantPush": True})

@app.route("/api/v1/web/devices/<device_id>/send-raw", methods=["POST"])
def web_send_raw(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    if device_id not in storage_mqtt.get_user_device_ids(user):
        return jsonify({"error": "Bạn không có quyền với thiết bị này"}), 403

    data          = request.get_json(silent=True) or {}
    raw_us        = data.get("rawUs", [])
    frequency_khz = int(data.get("frequencyKhz", 38))
    profile_id    = data.get("profileId", "raw_profile")

    if not raw_us:
        return jsonify({"error": "Dữ liệu rawUs trống"}), 400

    cmd_payload = {"profileId": profile_id, "frequencyKhz": frequency_khz, "rawUs": raw_us}
    cmd = storage_mqtt.add_command(device_id, "SEND_RAW", cmd_payload)
    publish_mqtt_command(device_id, cmd)
    return jsonify({"success": True, "command": cmd, "instantPush": True})

@app.route("/api/v1/web/devices/<device_id>/profiles", methods=["GET"])
def web_get_profiles(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    profiles = storage_mqtt.get_profiles(device_id)
    return jsonify({"profiles": profiles})

@app.route("/api/v1/web/devices/<device_id>/profiles/create", methods=["POST"])
def web_create_profile(device_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    if device_id not in storage_mqtt.get_user_device_ids(user):
        return jsonify({"error": "Bạn không có quyền với thiết bị này"}), 403

    data    = request.get_json(silent=True) or {}
    name    = data.get("name", "").strip()
    profile = storage_mqtt.create_profile(device_id, name)
    return jsonify({"success": True, "profile": profile})

@app.route("/api/v1/web/commands/<command_id>/status", methods=["GET"])
def web_command_status(command_id):
    user, err_resp, err_code = require_user()
    if err_resp:
        return err_resp, err_code
    cmd = storage_mqtt.get_command_status(command_id)
    if not cmd:
        return jsonify({"error": "Command not found"}), 404
    return jsonify({"command": cmd})

# ============================================================
# Static Web UI (MQTT Version)
# ============================================================
@app.route("/")
def index():
    return send_from_directory(WEB_MQTT_DIR, "index.html")

@app.route("/<path:filename>")
def serve_static(filename):
    filepath = os.path.join(WEB_MQTT_DIR, filename)
    if os.path.isfile(filepath):
        return send_from_directory(WEB_MQTT_DIR, filename)
    return jsonify({"error": "Not found"}), 404

if __name__ == "__main__":
    host = os.getenv("HOST", "0.0.0.0")
    port = int(os.getenv("PORT", 3001))
    print("==================================================")
    print("  AC CONTROLLER BACKEND MQTT SERVER RUNNING (v0.3)")
    print(f"  API Endpoint: http://{host}:{port}/api/v1")
    print(f"  Web Dashboard MQTT: http://localhost:{port}")
    print(f"  MQTT Broker: {MQTT_BROKER_HOST}:{MQTT_BROKER_PORT}")
    print("==================================================")
    app.run(host=host, port=port, debug=True)
