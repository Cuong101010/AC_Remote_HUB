// =====================================================
//  AC Remote Hub — app.js
//  Auth + multi-user + device management
// =====================================================

// ── Global auth state ─────────────────────────────
let currentUser = null;       // { userId, username, displayName, sessionToken }
let sessionToken = null;

// ── App state ─────────────────────────────────────
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
    localStorage.setItem("session_token", user.sessionToken);
    localStorage.setItem("user_info", JSON.stringify({
        userId: user.userId, username: user.username, displayName: user.displayName
    }));
}

function clearSession() {
    currentUser  = null;
    sessionToken = null;
    localStorage.removeItem("session_token");
    localStorage.removeItem("user_info");
}

function loadSavedSession() {
    const token = localStorage.getItem("session_token");
    const info  = localStorage.getItem("user_info");
    if (token && info) {
        try {
            const u = JSON.parse(info);
            currentUser  = { ...u, sessionToken: token };
            sessionToken = token;
            return true;
        } catch (e) { /* ignore */ }
    }
    return false;
}

// ── API fetch wrapper ──────────────────────────────
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
    text.textContent = online ? "Online" : "Mất kết nối";
}

// =====================================================
//  AUTH SCREENS
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
    }
}

function togglePasswordVisibility(inputId, btn) {
    const input = document.getElementById(inputId);
    if (!input) return;
    const isPassword = input.type === "password";
    input.type = isPassword ? "text" : "password";
    
    const eyeIcon = btn.querySelector(".eye-icon");
    const eyeOffIcon = btn.querySelector(".eye-off-icon");
    if (eyeIcon && eyeOffIcon) {
        eyeIcon.classList.toggle("hidden", isPassword);
        eyeOffIcon.classList.toggle("hidden", !isPassword);
    }
}
window.togglePasswordVisibility = togglePasswordVisibility;

function showAuthError(id, message) {
    const el = document.getElementById(id);
    if (!el) return;
    el.textContent = message;
    el.classList.remove("hidden");
}
function hideAuthError(id) {
    const el = document.getElementById(id);
    if (el) el.classList.add("hidden");
}

function showAuthScreen() {
    document.getElementById("auth-screen").classList.remove("hidden");
    document.getElementById("app-screen").classList.add("hidden");
}

function showAppScreen() {
    document.getElementById("auth-screen").classList.add("hidden");
    document.getElementById("app-screen").classList.remove("hidden");
    document.getElementById("user-display-name").textContent =
        currentUser?.displayName || currentUser?.username || "--";
}

// LOGIN
document.getElementById("login-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    hideAuthError("login-error");
    const username = document.getElementById("login-username").value.trim();
    const password = document.getElementById("login-password").value;
    const btn      = document.getElementById("btn-login");
    btn.disabled   = true;
    btn.querySelector("span").textContent = "Đang đăng nhập...";

    const res = await apiFetch("/api/v1/auth/login", {
        method: "POST",
        body: JSON.stringify({ username, password })
    });

    btn.disabled = false;
    btn.querySelector("span").textContent = "Đăng Nhập";

    if (res && res.success) {
        saveSession(res.user);
        showAppScreen();
        initApp();
    } else {
        showAuthError("login-error", res?.error || "Đăng nhập thất bại");
    }
});

// REGISTER
document.getElementById("register-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    hideAuthError("register-error");
    const displayName = document.getElementById("reg-displayname").value.trim();
    const username    = document.getElementById("reg-username").value.trim();
    const password    = document.getElementById("reg-password").value;
    const password2   = document.getElementById("reg-password2").value;

    if (password !== password2) {
        showAuthError("register-error", "Mật khẩu xác nhận không khớp");
        return;
    }
    const btn = document.getElementById("btn-register");
    btn.disabled = true;
    btn.querySelector("span").textContent = "Đang tạo tài khoản...";

    const res = await apiFetch("/api/v1/auth/register", {
        method: "POST",
        body: JSON.stringify({ username, password, displayName })
    });

    btn.disabled = false;
    btn.querySelector("span").textContent = "Tạo Tài Khoản";

    if (res && res.success) {
        saveSession(res.user);
        showAppScreen();
        initApp();
    } else {
        showAuthError("register-error", res?.error || "Tạo tài khoản thất bại");
    }
});

// LOGOUT
document.getElementById("btn-logout").addEventListener("click", async () => {
    await apiFetch("/api/v1/auth/logout", { method: "POST" });
    clearSession();
    state = { ...state, devices: [], profiles: [], activeDeviceId: null, activeProfileId: null };
    showAuthScreen();
});

// =====================================================
//  NAVIGATION TABS
// =====================================================

const navButtons = document.querySelectorAll(".nav-btn");
const tabPages   = document.querySelectorAll(".tab-page");

navButtons.forEach(btn => {
    btn.addEventListener("click", () => {
        const targetTab = btn.getAttribute("data-tab");
        switchToTab(targetTab);
    });
});

function switchToTab(targetTab) {
    navButtons.forEach(b => {
        b.classList.toggle("active", b.getAttribute("data-tab") === targetTab);
    });
    tabPages.forEach(p => {
        p.classList.toggle("active", p.id === targetTab);
    });
    if (targetTab === "tab-learning") loadLearnedProfiles();
    else if (targetTab === "tab-devices") loadDevices();
}

// =====================================================
//  TOAST
// =====================================================

function showToast(message, isError = false) {
    const toast        = document.getElementById("toast");
    const toastMessage = document.getElementById("toast-message");
    toastMessage.textContent = message;
    toast.className = `toast${isError ? " error" : ""}`;
    setTimeout(() => { toast.className = "toast hidden"; }, 3500);
}

// =====================================================
//  DEVICES
// =====================================================

async function loadDevices() {
    const data = await apiFetch("/api/v1/web/devices");
    if (!data || !data.devices) return;

    state.devices = data.devices;
    renderDevicesList();

    if (!state.activeDeviceId && state.devices.length > 0) {
        selectDevice(state.devices[0].deviceId);
    } else if (state.activeDeviceId) {
        const current = state.devices.find(d => d.deviceId === state.activeDeviceId);
        if (current) updateDeviceUI(current);
    }

    updateRemoteVisibility();
}

function updateRemoteVisibility() {
    const noDevState    = document.getElementById("no-device-state");
    const remoteContent = document.getElementById("remote-content");
    if (state.devices.length === 0 || !state.activeDeviceId) {
        noDevState?.classList.remove("hidden");
        remoteContent?.classList.add("hidden");
    } else {
        noDevState?.classList.add("hidden");
        remoteContent?.classList.remove("hidden");
    }
}

function selectDevice(deviceId) {
    state.activeDeviceId = deviceId;
    const dev = state.devices.find(d => d.deviceId === deviceId);
    if (dev) {
        updateDeviceUI(dev);
        showToast(`Đã chọn: ${dev.deviceId}`);
        loadProfiles();
    }
    renderDevicesList();
    updateRemoteVisibility();
}

function updateDeviceUI(dev) {
    const subtitle = document.getElementById("active-device-subtitle");
    const name     = document.getElementById("esp-device-name");
    const meta     = document.getElementById("esp-meta-info");
    const dot      = document.getElementById("esp-status-dot");
    if (subtitle) subtitle.textContent = `${dev.deviceId} (${dev.online ? "Online" : "Offline"})`;
    if (name)     name.textContent     = dev.deviceId;
    if (meta)     meta.textContent     = `IP: ${dev.ip || "--"} | RSSI: ${dev.rssi || "--"} dBm`;
    if (dot)      dot.className        = `status-indicator${dev.online ? "" : " offline"}`;
}

function renderDevicesList() {
    const grid = document.getElementById("device-grid");
    if (!grid) return;
    if (state.devices.length === 0) {
        grid.innerHTML = `<div class="empty-state">Chưa có thiết bị nào. Hãy ghép nối ESP32 bên dưới.</div>`;
        return;
    }
    grid.innerHTML = state.devices.map(dev => `
        <div class="device-item-card${dev.deviceId === state.activeDeviceId ? " active-device" : ""}">
            <div class="device-meta">
                <div class="device-status-row">
                    <span class="status-indicator${dev.online ? "" : " offline"}"></span>
                    <h4>${dev.deviceId}</h4>
                    ${dev.paired ? '<span class="paired-badge">✓ Ghép nối</span>' : ""}
                </div>
                <p>IP: ${dev.ip || "--"} &nbsp;|&nbsp; WiFi: ${dev.ssid || "--"} (${dev.rssi || 0} dBm)</p>
                <p>FW: v${dev.firmwareVersion || "0.1.0"} &nbsp;|&nbsp; Heap: ${dev.freeHeap || 0}B</p>
            </div>
            <button class="btn-secondary${dev.deviceId === state.activeDeviceId ? " btn-active" : ""}"
                    onclick="window.selectDeviceApp('${dev.deviceId}')">
                ${dev.deviceId === state.activeDeviceId ? "Đang Chọn" : "Chọn"}
            </button>
        </div>
    `).join("");
}

window.selectDeviceApp = (id) => selectDevice(id);

// PAIR DEVICE
document.getElementById("btn-pair-submit").addEventListener("click", async () => {
    const codeInput = document.getElementById("pair-code-input");
    const code = codeInput.value.trim();
    if (!code) { showToast("Vui lòng nhập mã ghép nối", true); return; }

    const res = await apiFetch("/api/v1/web/pair", {
        method: "POST",
        body: JSON.stringify({ code })
    });

    if (res && res.success) {
        showToast("Ghép nối thiết bị thành công! 🎉");
        codeInput.value = "";
        loadDevices();
    } else {
        showToast(res?.error || "Ghép nối thất bại", true);
    }
});

document.getElementById("btn-refresh-devices").addEventListener("click", () => loadDevices());

// =====================================================
//  PROFILES
// =====================================================

async function loadProfiles() {
    if (!state.activeDeviceId) return;
    const data = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/profiles`);
    if (!data || !data.profiles) return;
    state.profiles = data.profiles;
    renderProfilesList();
}

function renderProfilesList() {
    const select = document.getElementById("select-ac-profile");
    if (!select) return;

    if (state.profiles.length === 0) {
        select.innerHTML = `<option value="">-- Chưa có điều hòa --</option>`;
        state.activeProfileId = null;
        applyProfileSettings(null);
        return;
    }

    select.innerHTML = state.profiles.map(p => {
        const name = p.name || p.profileId;
        let displayName;
        if (name.includes("(")) {
            displayName = name;
        } else if (p.protocol && p.protocol !== "UNKNOWN") {
            displayName = `${name} (${p.protocol})`;
        } else {
            displayName = `${name} (Chưa nhận diện)`;
        }
        return `
        <option value="${p.profileId}" ${p.profileId === state.activeProfileId ? "selected" : ""}>
            ❄️ ${displayName}
        </option>
    `;
    }).join("");

    if (!state.activeProfileId && state.profiles.length > 0) {
        switchProfile(state.profiles[0].profileId);
    } else if (state.activeProfileId) {
        const prof = state.profiles.find(p => p.profileId === state.activeProfileId);
        if (prof) {
            applyProfileSettings(prof);
        } else if (state.profiles.length > 0) {
            switchProfile(state.profiles[0].profileId);
        } else {
            state.activeProfileId = null;
            applyProfileSettings(null);
        }
    }
}

document.getElementById("select-ac-profile").addEventListener("change", e => switchProfile(e.target.value));

function switchProfile(profileId) {
    if (!profileId) return;
    state.activeProfileId = profileId;
    const prof = state.profiles.find(p => p.profileId === profileId);
    if (prof) {
        applyProfileSettings(prof);
        showToast(`Đã chuyển sang: ${prof.name || profileId}`);
    }
}

function applyProfileSettings(prof) {
    const badge = document.getElementById("auto-protocol-text");
    if (prof && prof.protocol && prof.protocol !== "UNKNOWN") {
        state.acState.protocol = prof.protocol;
        if (badge) badge.textContent = `✨ Tự động nhận diện Hãng: ${prof.protocol}`;
    } else if (prof && prof.controlType === "RAW") {
        state.acState.protocol = "RAW";
        if (badge) badge.textContent = `✨ Chế độ phát Tín Hiệu RAW (Hãng lạ)`;
    } else {
        state.acState.protocol = "";
        if (badge) badge.textContent = `✨ Chưa nhận diện Hãng (Hãy bấm remote gốc)`;
    }
    const learnInput = document.getElementById("learn-profile-id");
    if (learnInput && prof) learnInput.value = prof.profileId;
    renderAcStateUI();
}

// ADD PROFILE
document.getElementById("btn-add-profile").addEventListener("click", async () => {
    if (!state.activeDeviceId) { showToast("Chưa chọn thiết bị ESP32!", true); return; }

    const count = state.profiles.length + 1;
    const name  = prompt(`Nhập tên điều hòa mới:`, `Điều Hòa #${count}`);
    if (name === null) return;

    const res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/profiles/create`, {
        method: "POST",
        body: JSON.stringify({ name: name.trim() || `Điều Hòa #${count}` })
    });

    if (res && res.success && res.profile) {
        showToast(`Đã tạo ${res.profile.name}! Kích hoạt quét tự động...`);
        state.activeProfileId = res.profile.profileId;
        await loadProfiles();
        triggerAutoLearnForProfile(res.profile.profileId);
    } else {
        showToast("Không thể tạo điều hòa mới", true);
    }
});

// DELETE PROFILE
document.getElementById("btn-delete-profile").addEventListener("click", async () => {
    if (!state.activeProfileId) {
        showToast("Chưa chọn điều hòa nào để xóa!", true);
        return;
    }

    const currentProf = state.profiles.find(p => p.profileId === state.activeProfileId);
    const profName = currentProf ? (currentProf.name || currentProf.profileId) : state.activeProfileId;

    if (!confirm(`Bạn có chắc chắn muốn xóa "${profName}" không?`)) {
        return;
    }

    const res = await apiFetch(`/api/v1/web/profiles/${state.activeProfileId}`, {
        method: "DELETE"
    });

    if (res && res.success) {
        showToast(`Đã xóa "${profName}" thành công!`);
        state.activeProfileId = null;
        await loadProfiles();
    } else {
        showToast(res?.error || "Không thể xóa điều hòa này", true);
    }
});

async function triggerAutoLearnForProfile(profileId) {
    const res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/learn`, {
        method: "POST",
        body: JSON.stringify({ profileId, expectedAction: "POWER_ON_COOL_26", timeoutSeconds: 45 })
    });
    if (res && res.success) {
        state.learningSession = { active: true, commandId: res.command.id, profileId };
        const box = document.getElementById("learning-status-box");
        if (box) box.classList.remove("hidden");
        showToast("BẬT QUÉT REMOTE GỐC: Hãy bấm 1 nút trên remote hướng về ESP32!");
        pollLearningStatus(profileId);
    }
}

// =====================================================
//  AC REMOTE UI
// =====================================================

function renderAcStateUI() {
    const tv = document.getElementById("temp-value");
    if (tv) tv.textContent = state.acState.temperature;

    const pw = document.getElementById("btn-power");
    if (pw) pw.classList.toggle("on", state.acState.power);

    document.querySelectorAll(".mode-selector .seg-btn").forEach(btn => {
        btn.classList.toggle("active", btn.getAttribute("data-mode") === state.acState.mode);
    });
    document.querySelectorAll(".fan-selector .seg-btn").forEach(btn => {
        btn.classList.toggle("active", btn.getAttribute("data-fan") === state.acState.fan);
    });

    const swV = document.getElementById("select-swing-v");
    const swH = document.getElementById("select-swing-h");
    const sp  = document.getElementById("select-protocol");
    if (swV) swV.value = state.acState.swingV;
    if (swH) swH.value = state.acState.swingH;
    if (sp) {
        if (state.acState.protocol && !Array.from(sp.options).some(opt => opt.value === state.acState.protocol)) {
            const newOpt = document.createElement("option");
            newOpt.value = state.acState.protocol;
            newOpt.textContent = state.acState.protocol;
            sp.appendChild(newOpt);
        }
        sp.value = state.acState.protocol;
    }

    const chkTurbo = document.getElementById("chk-turbo");
    const chkQuiet = document.getElementById("chk-quiet");
    const chkEcono = document.getElementById("chk-econo");
    const chkLight = document.getElementById("chk-light");
    if (chkTurbo) chkTurbo.checked = state.acState.turbo;
    if (chkQuiet) chkQuiet.checked = state.acState.quiet;
    if (chkEcono) chkEcono.checked = state.acState.econo;
    if (chkLight) chkLight.checked = state.acState.light;
}

// Temperature Controls
document.getElementById("btn-temp-minus").addEventListener("click", () => {
    if (state.acState.temperature > 16) { state.acState.temperature--; renderAcStateUI(); }
});
document.getElementById("btn-temp-plus").addEventListener("click", () => {
    if (state.acState.temperature < 30) { state.acState.temperature++; renderAcStateUI(); }
});
document.getElementById("btn-power").addEventListener("click", () => {
    state.acState.power = !state.acState.power; renderAcStateUI();
});

// Mode & Fan buttons
document.querySelectorAll(".mode-selector .seg-btn").forEach(btn => {
    btn.addEventListener("click", () => { state.acState.mode = btn.getAttribute("data-mode"); renderAcStateUI(); });
});
document.querySelectorAll(".fan-selector .seg-btn").forEach(btn => {
    btn.addEventListener("click", () => { state.acState.fan = btn.getAttribute("data-fan"); renderAcStateUI(); });
});

// Selects & Checkboxes
document.getElementById("select-swing-v").addEventListener("change", e => state.acState.swingV = e.target.value);
document.getElementById("select-swing-h").addEventListener("change", e => state.acState.swingH = e.target.value);
document.getElementById("select-protocol").addEventListener("change", e => state.acState.protocol = e.target.value);
document.getElementById("chk-turbo").addEventListener("change", e => state.acState.turbo = e.target.checked);
document.getElementById("chk-quiet").addEventListener("change", e => state.acState.quiet = e.target.checked);
document.getElementById("chk-econo").addEventListener("change", e => state.acState.econo = e.target.checked);
document.getElementById("chk-light").addEventListener("change", e => state.acState.light = e.target.checked);

// SEND COMMAND
document.getElementById("btn-send-ac-state").addEventListener("click", async () => {
    if (!state.activeDeviceId) { showToast("Chưa chọn thiết bị ESP32!", true); return; }
    showToast("Đang gửi lệnh tới ESP32...");
    const payload = { ...state.acState, profileId: state.activeProfileId || "default_profile" };
    const res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/control`, {
        method: "POST",
        body: JSON.stringify(payload)
    });
    if (res && res.success) {
        showToast(`Đã xếp hàng lệnh. Chờ ESP32 nhận...`);
        pollCommandStatus(res.command.id);
    } else {
        showToast(res?.error || "Gửi lệnh thất bại", true);
    }
});

async function pollCommandStatus(commandId) {
    let attempts = 0;
    const interval = setInterval(async () => {
        attempts++;
        const res = await apiFetch(`/api/v1/web/commands/${commandId}/status`);
        if (res && res.command) {
            const status = res.command.ackStatus || res.command.status;
            if (status === "completed") {
                clearInterval(interval);
                showToast(`ESP32 đã thực thi lệnh OK ✅`);
            } else if (status === "failed") {
                clearInterval(interval);
                showToast(`Lệnh thất bại: ${res.command.ackMessage || ""}`, true);
            }
        }
        if (attempts > 15) clearInterval(interval);
    }, 1500);
}

// =====================================================
//  IR LEARNING
// =====================================================

document.getElementById("btn-start-learn").addEventListener("click", async () => {
    if (!state.activeDeviceId) { showToast("Chưa chọn thiết bị ESP32!", true); return; }

    const profileId      = document.getElementById("learn-profile-id").value.trim()       || "my_ac_remote";
    const expectedAction = document.getElementById("learn-expected-action").value.trim()   || "POWER_ON_COOL_26";

    const res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/learn`, {
        method: "POST",
        body: JSON.stringify({ profileId, expectedAction, timeoutSeconds: 45 })
    });

    if (res && res.success) {
        state.learningSession = { active: true, commandId: res.command.id, profileId };
        document.getElementById("learning-status-box").classList.remove("hidden");
        showToast("Đã kích hoạt chế độ học IR trên ESP32.");
        pollLearningStatus(profileId, res.command.id);
    } else {
        showToast("Không thể bắt đầu học IR", true);
    }
});

document.getElementById("btn-cancel-learn").addEventListener("click", async () => {
    if (!state.activeDeviceId) return;
    await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/cancel-learn`, { method: "POST" });
    state.learningSession.active = false;
    document.getElementById("learning-status-box").classList.add("hidden");
    showToast("Đã hủy chế độ học IR.");
});

async function pollLearningStatus(profileId, commandId) {
    let count = 0;
    const timer = setInterval(async () => {
        count++;
        if (!state.learningSession.active) { clearInterval(timer); return; }

        if (commandId) {
            const statusRes = await apiFetch(`/api/v1/web/commands/${commandId}/status`);
            if (statusRes && statusRes.command) {
                const cmd = statusRes.command;
                const status = (cmd.status || "").toLowerCase();
                const ackStatus = (cmd.ackStatus || "").toLowerCase();

                if (status === "success" || status === "completed" || ackStatus === "completed") {
                    clearInterval(timer);
                    state.learningSession.active = false;
                    document.getElementById("learning-status-box").classList.add("hidden");
                    await loadProfiles();
                    if (state.activeProfileId !== profileId) {
                        switchProfile(profileId);
                    }
                    await loadLearnedProfiles();
                    const proto = cmd.result?.protocol || "NATIVE";
                    showToast(`🎉 Học lệnh thành công: Hãng ${proto}!`);
                    return;
                } else if (status === "failed" || ackStatus === "failed" || status === "timeout") {
                    clearInterval(timer);
                    state.learningSession.active = false;
                    document.getElementById("learning-status-box").classList.add("hidden");
                    showToast("Học lệnh thất bại hoặc đã hết thời gian chờ.", true);
                    return;
                }
            }
        }

        if (count > 32) {
            clearInterval(timer);
            state.learningSession.active = false;
            document.getElementById("learning-status-box").classList.add("hidden");
            await loadProfiles();
            await loadLearnedProfiles();
            showToast("Hết thời gian học IR (Timeout).", true);
        }
    }, 1500);
}

async function loadLearnedProfiles() {
    if (!state.activeDeviceId) return;
    const data = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/profiles`);
    const list = document.getElementById("signals-list");
    if (!list) return;

    if (!data || !data.profiles || data.profiles.length === 0) {
        list.innerHTML = `<div class="empty-state">Chưa có dữ liệu lệnh IR nào được học.</div>`;
        return;
    }

    let html = "";
    data.profiles.forEach(prof => {
        (prof.signals || []).forEach(sig => {
            const actionName = sig.action || sig.expectedAction;
            const addrHex = sig.address ? `0x${sig.address.toString(16).toUpperCase()}` : "";
            const cmdHex = sig.commandCode ? `0x${sig.commandCode.toString(16).toUpperCase()}` : "";
            const extraDetails = [
                addrHex ? `Addr: <strong>${addrHex}</strong>` : "",
                cmdHex ? `Cmd: <strong>${cmdHex}</strong>` : "",
                sig.repeatCount ? `Repeat: <strong>${sig.repeatCount}</strong>` : ""
            ].filter(Boolean).join(" | ");

            html += `
                <div class="signal-item">
                    <div class="signal-info">
                        <h4>${actionName}</h4>
                        <p>Protocol: <strong>${sig.protocol}</strong> | Bits: ${sig.bits || "--"}${extraDetails ? " | " + extraDetails : ""}</p>
                        <p>Code: <code style="color: var(--accent);">${sig.code || sig.stateHex || "RAW Signal"}</code></p>
                    </div>
                    <div class="signal-actions" style="display: flex; gap: 6px; align-items: center;">
                        <button class="btn-secondary" onclick="window.sendLearnedSignal('${prof.profileId}', '${actionName}')">
                            Phát IR
                        </button>
                        <button class="btn-secondary danger" onclick="window.deleteLearnedSignal('${prof.profileId}', '${actionName}')">
                            🗑️ Xóa
                        </button>
                    </div>
                </div>
            `;
        });
    });
    list.innerHTML = html || `<div class="empty-state">Chưa có lệnh IR nào được học.</div>`;
}

window.sendLearnedSignal = async (profileId, actionName) => {
    if (!state.activeDeviceId) {
        showToast("Chưa chọn thiết bị ESP32!", true);
        return;
    }

    let signalToTransmit = null;
    const targetProf = state.profiles.find(p => p.profileId === profileId);
    if (targetProf && targetProf.signals) {
        signalToTransmit = targetProf.signals.find(s => (s.action === actionName || s.expectedAction === actionName));
    }

    if (!signalToTransmit) {
        const data = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/profiles`);
        if (data && data.profiles) {
            const p = data.profiles.find(x => x.profileId === profileId);
            if (p && p.signals) {
                signalToTransmit = p.signals.find(s => (s.action === actionName || s.expectedAction === actionName));
            }
        }
    }

    showToast(`Đang gửi tín hiệu IR cho lệnh: ${actionName}...`);

    let res = null;
    if (signalToTransmit) {
        res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/send-raw`, {
            method: "POST",
            body: JSON.stringify({
                profileId: profileId,
                rawUs: signalToTransmit.rawUs || [],
                protocol: signalToTransmit.protocol || "",
                code: signalToTransmit.code || "",
                bits: signalToTransmit.bits || 0,
                address: signalToTransmit.address || 0,
                commandCode: signalToTransmit.commandCode || 0,
                repeatCount: signalToTransmit.repeatCount || 0,
                frequencyKhz: 38
            })
        });
    } else if (signalToTransmit && signalToTransmit.controlType === "NATIVE" && signalToTransmit.commonState) {
        const payload = {
            ...signalToTransmit.commonState,
            profileId: profileId,
            protocol: signalToTransmit.protocol
        };
        res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/control`, {
            method: "POST",
            body: JSON.stringify(payload)
        });
    } else {
        showToast("Không tìm thấy dữ liệu tín hiệu của lệnh này", true);
        return;
    }

    if (res && res.success) {
        showToast(`Đã gửi lệnh phát IR tới ESP32!`);
        pollCommandStatus(res.command.id);
    } else {
        showToast(res?.error || "Không thể gửi lệnh phát IR", true);
    }
};

window.deleteLearnedSignal = async (profileId, actionName) => {
    if (!state.activeDeviceId) {
        showToast("Chưa chọn thiết bị ESP32!", true);
        return;
    }

    if (!confirm(`Bạn có chắc chắn muốn xóa lệnh "${actionName}" không?`)) {
        return;
    }

    const res = await apiFetch(`/api/v1/web/devices/${state.activeDeviceId}/profiles/${profileId}/signals/${encodeURIComponent(actionName)}`, {
        method: "DELETE"
    });

    if (res && res.success) {
        showToast(`Đã xóa lệnh "${actionName}" thành công!`);
        await loadProfiles();
        loadLearnedProfiles();
    } else {
        showToast(res?.error || "Không thể xóa lệnh này", true);
    }
};

// =====================================================
//  INIT
// =====================================================

let autoRefreshTimer = null;

function initApp() {
    renderAcStateUI();
    loadDevices();
    if (autoRefreshTimer) clearInterval(autoRefreshTimer);
    autoRefreshTimer = setInterval(loadDevices, 5000);
}

// Entry point — check saved session
(async function boot() {
    if (loadSavedSession()) {
        // Verify session is still valid
        const me = await apiFetch("/api/v1/auth/me");
        if (me && !me.error) {
            // Refresh displayName from server
            currentUser.displayName = me.displayName || me.username;
            showAppScreen();
            initApp();
            return;
        }
    }
    clearSession();
    showAuthScreen();
})();
