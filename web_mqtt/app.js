// =====================================================
//  AC Remote Hub — app.js (v0.3 MQTT Instant Control)
// =====================================================

let currentUser = null;
let sessionToken = null;

let state = {
    devices: [],
    profiles: [],
    activeDeviceId: null,
    activeProfileId: null,
    acState: {
        power: true, temperature: 26, mode: "cool",
        fan: "auto", swingV: "off", swingH: "off",
        turbo: false, quiet: false, econo: false, light: false,
        protocol: ""
    },
    learningSession: { active: false, commandId: null, profileId: null }
};

// =====================================================
//  AUTH HELPERS
// =====================================================

function saveSession(user) {
    currentUser  = user;
    sessionToken = user.sessionToken;
    localStorage.setItem("session_token_mqtt", user.sessionToken);
    localStorage.setItem("user_info_mqtt", JSON.stringify({
        userId: user.userId, username: user.username, displayName: user.displayName
    }));
}

function clearSession() {
    currentUser  = null;
    sessionToken = null;
    localStorage.removeItem("session_token_mqtt");
    localStorage.removeItem("user_info_mqtt");
}

function loadSavedSession() {
    const token = localStorage.getItem("session_token_mqtt");
    const info  = localStorage.getItem("user_info_mqtt");
    if (token && info) {
        try {
            const u = JSON.parse(info);
            currentUser  = { ...u, sessionToken: token };
            sessionToken = token;
            return true;
        } catch (e) {}
    }
    return false;
}

async function apiFetch(endpoint, options = {}) {
    const headers = {
        "Content-Type": "application/json",
        ...(sessionToken ? { "Authorization": `Bearer ${sessionToken}` } : {}),
        ...(options.headers || {})
    };

    try {
        const response = await fetch(endpoint, { ...options, headers });
        updateBackendStatus(true);
        if (response.status === 204) return null;
        const data = await response.json();
        data.__status = response.status;
        return data;
    } catch (err) {
        updateBackendStatus(false);
        console.error("API Fetch error:", err);
        return null;
    }
}

function updateBackendStatus(online) {
    const pill = document.getElementById("backend-status-pill");
    const text = document.getElementById("backend-status-text");
    if (!pill || !text) return;
    pill.className = `status-pill${online ? "" : " offline"}`;
    text.textContent = online ? "MQTT Online" : "Mất kết nối";
}

// =====================================================
//  AUTH UI
// =====================================================

function showAuthTab(tab) {
    const loginForm    = document.getElementById("login-form");
    const registerForm = document.getElementById("register-form");
    const loginBtn     = document.getElementById("tab-login-btn");
    const registerBtn  = document.getElementById("tab-register-btn");

    if (tab === "login") {
        loginForm.classList.remove("hidden");
        registerForm.classList.add("hidden");
        loginBtn.classList.add("active");
        registerBtn.classList.remove("active");
    } else {
        loginForm.classList.add("hidden");
        registerForm.classList.remove("hidden");
        loginBtn.classList.remove("active");
        registerBtn.classList.add("active");
    }
}

function togglePasswordVisibility(inputId, btn) {
    const input = document.getElementById(inputId);
    if (!input) return;
    if (input.type === "password") {
        input.type = "text";
    } else {
        input.type = "password";
    }
}

async function handleLogin(e) {
    e.preventDefault();
    const username = document.getElementById("login-username").value.trim();
    const password = document.getElementById("login-password").value;
    const errDiv   = document.getElementById("login-error");
    errDiv.classList.add("hidden");

    const res = await apiFetch("/api/v1/auth/login", {
        method: "POST", body: JSON.stringify({ username, password })
    });

    if (res && res.success) {
        saveSession(res.user);
        initAppScreen();
    } else {
        errDiv.textContent = (res && res.error) || "Đăng nhập thất bại";
        errDiv.classList.remove("hidden");
    }
}

async function handleRegister(e) {
    e.preventDefault();
    const displayName = document.getElementById("reg-displayname").value.trim();
    const username    = document.getElementById("reg-username").value.trim();
    const password    = document.getElementById("reg-password").value;
    const password2   = document.getElementById("reg-password2").value;
    const errDiv      = document.getElementById("register-error");
    errDiv.classList.add("hidden");

    if (password !== password2) {
        errDiv.textContent = "Mật khẩu xác nhận không khớp";
        errDiv.classList.remove("hidden");
        return;
    }

    const res = await apiFetch("/api/v1/auth/register", {
        method: "POST", body: JSON.stringify({ username, password, displayName })
    });

    if (res && res.success) {
        saveSession(res.user);
        initAppScreen();
    } else {
        errDiv.textContent = (res && res.error) || "Đăng ký thất bại";
        errDiv.classList.remove("hidden");
    }
}

function handleLogout() {
    apiFetch("/api/v1/auth/logout", { method: "POST" });
    clearSession();
    document.getElementById("app-screen").classList.add("hidden");
    document.getElementById("auth-screen").classList.remove("hidden");
}

// =====================================================
//  APP INITIALIZATION & NAVIGATION
// =====================================================

async function initAppScreen() {
    document.getElementById("auth-screen").classList.add("hidden");
    document.getElementById("app-screen").classList.remove("hidden");
    document.getElementById("user-display-name").textContent = currentUser.displayName || currentUser.username;

    await loadDevices();
    startPollingLoop();
}

function setupNavTabs() {
    document.querySelectorAll(".nav-btn").forEach(btn => {
        btn.addEventListener("click", () => {
            const targetTab = btn.getAttribute("data-tab");
            switchToTab(targetTab);
        });
    });
}

function switchToTab(tabId) {
    document.querySelectorAll(".nav-btn").forEach(b => b.classList.remove("active"));
    document.querySelectorAll(".tab-page").forEach(p => p.classList.remove("active"));

    const navBtn = document.querySelector(`.nav-btn[data-tab="${tabId}"]`);
    const page   = document.getElementById(tabId);
    if (navBtn) navBtn.classList.add("active");
    if (page)   page.classList.add("active");
}

// =====================================================
//  DEVICES & PAIRING
// =====================================================

async function loadDevices() {
    const res = await apiFetch("/api/v1/web/devices");
    if (!res || !res.devices) return;

    state.devices = res.devices;
    renderDeviceGrid();

    if (state.devices.length > 0) {
        if (!state.activeDeviceId || !state.devices.find(d => d.deviceId === state.activeDeviceId)) {
            setActiveDevice(state.devices[0].deviceId);
        } else {
            setActiveDevice(state.activeDeviceId);
        }
    } else {
        showNoDeviceState();
    }
}

function setActiveDevice(deviceId) {
    state.activeDeviceId = deviceId;
    const dev = state.devices.find(d => d.deviceId === deviceId);

    document.getElementById("no-device-state").classList.add("hidden");
    document.getElementById("remote-content").classList.remove("hidden");

    if (dev) {
        document.getElementById("esp-device-name").textContent = dev.deviceId;
        document.getElementById("active-device-subtitle").textContent = `MQTT Device: ${dev.deviceId}`;
        const dot = document.getElementById("esp-status-dot");
        if (dev.online) {
            dot.className = "status-indicator online";
            document.getElementById("esp-meta-info").textContent = `IP: ${dev.ip || 'Local'} | RSSI: ${dev.rssi || 0} dBm`;
        } else {
            dot.className = "status-indicator";
            document.getElementById("esp-meta-info").textContent = "MQTT Offline";
        }
    }

    loadProfiles(deviceId);
    fetchSensorData(deviceId);
}

function showNoDeviceState() {
    state.activeDeviceId = null;
    document.getElementById("no-device-state").classList.remove("hidden");
    document.getElementById("remote-content").classList.add("hidden");
    document.getElementById("active-device-subtitle").textContent = "Chưa ghép nối thiết bị";
}

function renderDeviceGrid() {
    const grid = document.getElementById("device-grid");
    if (!grid) return;

    if (state.devices.length === 0) {
        grid.innerHTML = `<div class="empty-state">Chưa có thiết bị nào. Hãy ghép nối thiết bị mới bằng mã 6 số.</div>`;
        return;
    }

    grid.innerHTML = state.devices.map(dev => `
        <div class="device-card ${dev.deviceId === state.activeDeviceId ? 'active' : ''}" onclick="setActiveDevice('${dev.deviceId}')">
            <div class="dev-header">
                <span class="status-indicator ${dev.online ? 'online' : ''}"></span>
                <strong>${dev.deviceId}</strong>
            </div>
            <div class="dev-body">
                <div>Firmware: ${dev.firmwareVersion || 'v0.3.0-MQTT'}</div>
                <div>MQTT Broker: broker.hivemq.com</div>
            </div>
        </div>
    `).join("");
}

async function handlePairSubmit() {
    const input = document.getElementById("pair-code-input");
    const code = input.value.trim();
    if (!code) return;

    const res = await apiFetch("/api/v1/web/pair", {
        method: "POST", body: JSON.stringify({ code })
    });

    if (res && res.success) {
        showToast("⚡ Ghép nối thiết bị MQTT thành công!");
        input.value = "";
        await loadDevices();
        setActiveDevice(res.device.deviceId);
    } else {
        showToast((res && res.error) || "Ghép nối thất bại", true);
    }
}

// =====================================================
//  PROFILES & CONTROL
// =====================================================

async function loadProfiles(deviceId) {
    if (!deviceId) return;
    const res = await apiFetch(`/api/v1/web/devices/${deviceId}/profiles`);
    if (!res || !res.profiles) return;

    state.profiles = res.profiles;
    renderProfileSelect();
}

function renderProfileSelect() {
    const select = document.getElementById("select-ac-profile");
    if (!select) return;

    if (state.profiles.length === 0) {
        select.innerHTML = `<option value="default_profile">Điều Hòa Mặc Định</option>`;
        return;
    }

    select.innerHTML = state.profiles.map(p => `
        <option value="${p.profileId}">${p.name}</option>
    `).join("");

    if (state.activeProfileId && state.profiles.find(p => p.profileId === state.activeProfileId)) {
        select.value = state.activeProfileId;
    } else {
        select.value = state.profiles[0].profileId;
        state.activeProfileId = state.profiles[0].profileId;
    }
}

async function handleAddProfile() {
    if (!state.activeDeviceId) return;
    const name = prompt("Nhập tên điều hòa mới (ví dụ: Điều hòa Phòng Khách):");
    if (!name) return;

    const res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/profiles/create`, {
        method: "POST", body: JSON.stringify({ name })
    });

    if (res && res.success) {
        showToast("Đã tạo hồ sơ điều hòa thành công");
        await loadProfiles(state.activeDeviceId);
    }
}

async function sendAcControlCommand() {
    if (!state.activeDeviceId) return;

    const payload = {
        profileId:   state.activeProfileId || "default_profile",
        protocol:    document.getElementById("select-protocol").value || "ELECTRA_AC",
        power:       state.acState.power,
        mode:        state.acState.mode,
        temperature: state.acState.temperature,
        fan:         state.acState.fan,
        swingV:      document.getElementById("select-swing-v").value,
        swingH:      document.getElementById("select-swing-h").value,
        turbo:       document.getElementById("chk-turbo").checked,
        quiet:       document.getElementById("chk-quiet").checked,
        econo:       document.getElementById("chk-econo").checked,
        light:       document.getElementById("chk-light").checked
    };

    const res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/control`, {
        method: "POST", body: JSON.stringify(payload)
    });

    if (res && res.success) {
        showToast("⚡ Đã bắn lệnh MQTT tức thì!");
    } else {
        showToast((res && res.error) || "Lỗi gửi lệnh", true);
    }
}

// =====================================================
//  DHT11 SENSOR WIDGET
// =====================================================

async function fetchSensorData(deviceId) {
    if (!deviceId) return;
    const res = await apiFetch(`/api/v1/web/devices/${deviceId}/sensor`);
    const widget = document.getElementById("temp-widget");
    const val = document.getElementById("temp-widget-value");

    if (res && res.sensor && res.sensor.temperature != null) {
        val.textContent = parseFloat(res.sensor.temperature).toFixed(1);
        widget.classList.remove("hidden");
    } else {
        widget.classList.add("hidden");
    }
}

// =====================================================
//  UI UTILS & EVENT LISTENERS
// =====================================================

function showToast(msg, isError = false) {
    const toast = document.getElementById("toast");
    const text  = document.getElementById("toast-message");
    if (!toast || !text) return;
    text.textContent = msg;
    toast.style.background = isError ? "#ef4444" : "#0284c7";
    toast.classList.remove("hidden");
    setTimeout(() => toast.classList.add("hidden"), 3000);
}

function setupControlListeners() {
    // Power button
    const pwrBtn = document.getElementById("btn-power");
    if (pwrBtn) {
        pwrBtn.addEventListener("click", () => {
            state.acState.power = !state.acState.power;
            pwrBtn.classList.toggle("on", state.acState.power);
            sendAcControlCommand();
        });
    }

    // Temperature +/- buttons
    document.getElementById("btn-temp-plus")?.addEventListener("click", () => {
        if (state.acState.temperature < 30) {
            state.acState.temperature++;
            document.getElementById("temp-value").textContent = state.acState.temperature;
            sendAcControlCommand();
        }
    });

    document.getElementById("btn-temp-minus")?.addEventListener("click", () => {
        if (state.acState.temperature > 16) {
            state.acState.temperature--;
            document.getElementById("temp-value").textContent = state.acState.temperature;
            sendAcControlCommand();
        }
    });

    // Mode buttons
    document.querySelectorAll(".mode-selector .seg-btn").forEach(btn => {
        btn.addEventListener("click", () => {
            document.querySelectorAll(".mode-selector .seg-btn").forEach(b => b.classList.remove("active"));
            btn.classList.add("active");
            state.acState.mode = btn.getAttribute("data-mode");
            sendAcControlCommand();
        });
    });

    // Fan buttons
    document.querySelectorAll(".fan-selector .seg-btn").forEach(btn => {
        btn.addEventListener("click", () => {
            document.querySelectorAll(".fan-selector .seg-btn").forEach(b => b.classList.remove("active"));
            btn.classList.add("active");
            state.acState.fan = btn.getAttribute("data-fan");
            sendAcControlCommand();
        });
    });

    // Main Send Button
    document.getElementById("btn-send-ac-state")?.addEventListener("click", sendAcControlCommand);
    document.getElementById("btn-pair-submit")?.addEventListener("click", handlePairSubmit);
    document.getElementById("btn-add-profile")?.addEventListener("click", handleAddProfile);
}

function startPollingLoop() {
    setInterval(() => {
        if (state.activeDeviceId) {
            fetchSensorData(state.activeDeviceId);
        }
    }, 5000);
}

document.addEventListener("DOMContentLoaded", () => {
    setupNavTabs();
    setupControlListeners();

    document.getElementById("login-form")?.addEventListener("submit", handleLogin);
    document.getElementById("register-form")?.addEventListener("submit", handleRegister);
    document.getElementById("btn-logout")?.addEventListener("click", handleLogout);

    if (loadSavedSession()) {
        initAppScreen();
    } else {
        document.getElementById("auth-screen").classList.remove("hidden");
    }
});
