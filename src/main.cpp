#include <Arduino.h>
#include <cinttypes>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

namespace {

constexpr uint32_t kDebugBaudRate = 115200;
constexpr uint32_t kUartBaudRate = 115200;
constexpr bool kEnableDebugLogs = false;

template <typename... Args>
void debugPrintf(const char *format, Args... args) {
  if constexpr (kEnableDebugLogs) {
    log_i(format, args...);
  }
}

void debugPrint(const char *message) {
  if constexpr (kEnableDebugLogs) {
    log_i("%s", message);
  }
}

void debugPrintln(const char *message) {
  if constexpr (kEnableDebugLogs) {
    log_i("%s", message);
  }
}

#ifndef UART1_RX_PIN
#define UART1_RX_PIN 18
#endif

#ifndef UART1_TX_PIN
#define UART1_TX_PIN 17
#endif

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 8
#endif

#ifndef STATUS_LED_ACTIVE_LEVEL
#define STATUS_LED_ACTIVE_LEVEL 0
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef AP_SSID
#define AP_SSID "ESP32C3-UART"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD "12345678"
#endif

#ifndef TCP_BRIDGE_PORT
#define TCP_BRIDGE_PORT 6638
#endif

constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiReconnectIntervalMs = 3000;
constexpr wifi_power_t kWifiTxPower = WIFI_POWER_8_5dBm;
constexpr uint32_t kUartBackpressureLogIntervalMs = 2000;
constexpr uint32_t kStatsLogIntervalMs = 10000;
constexpr uint32_t kDisconnectedBlinkPeriodMs = 140;
constexpr uint32_t kWifiBlinkPeriodMs = 700;
constexpr uint32_t kDataFlashWindowMs = 120;
constexpr uint8_t kUartRxFifoFullThreshold = 112;
constexpr size_t kIoChunkSize = 256;
constexpr size_t kPendingTcpToUartBytes = 12288;
constexpr size_t kPendingUartToTcpBytes = 20480;
constexpr size_t kUartDriverRxBufferSize = 8192;
constexpr size_t kUartDriverTxBufferSize = 4096;
constexpr uint8_t kMaxWiFiProfiles = 24;
constexpr uint16_t kHttpPort = 80;

/**
 * @brief UART1 framing and speed settings used by the bridge.
 */
struct UartSettings {
  uint32_t baudRate;
  uint8_t dataBits;
  char parity;
  uint8_t stopBits;
};

/**
 * @brief Persisted Wi-Fi credential slot.
 */
struct WiFiProfile {
  bool inUse;
  String ssid;
  String password;
};

/**
 * @brief Minimal Wi-Fi scan entry returned to the web UI.
 */
struct WiFiScanResult {
  String ssid;
  int32_t rssi;
  uint8_t channel;
  uint8_t encryption;
};

constexpr char kConfigPageHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-C3 串口配置</title>
  <style>
    :root { color-scheme: dark; }
    body { font-family: Arial, sans-serif; margin: 0; background: #111827; color: #e5e7eb; }
    main { max-width: 920px; margin: 0 auto; padding: 24px; }
    h1 { margin-top: 0; font-size: 1.6rem; }
    p { color: #cbd5e1; }
    .card { background: #1f2937; border-radius: 12px; padding: 20px; box-shadow: 0 10px 30px rgba(0,0,0,0.25); }
    .grid { display: grid; gap: 14px; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); }
    label { display: block; font-size: 0.95rem; margin-bottom: 6px; }
    .label-button { appearance: none; background: none; border: none; color: #93c5fd; padding: 0; margin: 0 0 6px; font-size: 0.95rem; cursor: pointer; text-align: left; }
    .label-button:hover { color: #bfdbfe; text-decoration: underline; }
    input, select, button { width: 100%; box-sizing: border-box; border-radius: 8px; border: 1px solid #374151; background: #111827; color: #f9fafb; padding: 10px 12px; font-size: 1rem; }
    button { background: #2563eb; border: none; font-weight: 600; cursor: pointer; }
    button:hover { background: #1d4ed8; }
    .status { margin-top: 14px; min-height: 1.5em; font-size: 0.95rem; }
    .current { margin-top: 18px; font-family: monospace; white-space: normal; overflow-wrap: anywhere; background: #0f172a; border-radius: 8px; padding: 12px; }
    .section { margin-top: 20px; }
    .profiles { margin-top: 16px; display: grid; gap: 10px; }
    .profile { background: #0f172a; border-radius: 8px; padding: 12px; }
    .profile-title { font-weight: 700; margin-bottom: 6px; }
    .profile-meta { font-size: 0.92rem; color: #cbd5e1; margin-bottom: 10px; }
    .profile-actions { display: flex; gap: 10px; }
    .profile-actions button { flex: 1; }
    .subtle { color: #94a3b8; font-size: 0.92rem; }
    .scan-list { margin-top: 16px; display: grid; gap: 10px; }
    .scan-item { background: #0f172a; border-radius: 8px; padding: 12px; cursor: pointer; border: 1px solid transparent; }
    .scan-item:hover { border-color: #2563eb; }
    .scan-title { font-weight: 700; margin-bottom: 6px; }
    .scan-meta { color: #cbd5e1; font-size: 0.92rem; }
    .modal-backdrop { position: fixed; inset: 0; display: none; align-items: center; justify-content: center; background: rgba(2, 6, 23, 0.72); padding: 20px; z-index: 1000; }
    .modal-backdrop.open { display: flex; }
    .modal { width: min(100%, 420px); background: #111827; border: 1px solid #374151; border-radius: 14px; box-shadow: 0 24px 60px rgba(0,0,0,0.45); padding: 18px; }
    .modal h3 { margin: 0 0 10px; font-size: 1.1rem; }
    .modal p { margin: 0; color: #d1d5db; line-height: 1.6; }
    .modal-actions { margin-top: 16px; display: flex; justify-content: flex-end; }
    .modal-actions button { width: auto; min-width: 88px; }
  </style>
</head>
<body>
  <main>
    <h1>ESP32-C3 串口桥控制台</h1>
    <p>查看并修改 TCP 串口桥当前使用的 UART1 参数。</p>
    <section class="card">
      <form id="uart-form">
        <div class="grid">
          <div>
            <button type="button" class="label-button" data-help-title="波特率" data-help-text="波特率表示串口每秒传输的符号数量。两端设备的波特率必须一致，否则会出现乱码或完全无法通信。常用值有 9600、115200、230400。">常用波特率</button>
            <select id="baudRatePreset">
              <option value="9600">9600</option>
              <option value="19200">19200</option>
              <option value="38400">38400</option>
              <option value="57600">57600</option>
              <option value="115200">115200</option>
              <option value="230400">230400</option>
              <option value="460800">460800</option>
              <option value="921600">921600</option>
              <option value="custom">自定义</option>
            </select>
          </div>
          <div>
            <button type="button" class="label-button" data-help-title="波特率" data-help-text="这里可以手动输入任意波特率，用于和外部设备保持一致。若输入的是常用值，上面的下拉框会自动同步。">波特率</button>
            <input id="baudRate" name="baudRate" type="number" min="300" step="1" placeholder="可手动输入任意波特率" required>
          </div>
          <div>
            <button type="button" class="label-button" data-help-title="数据位" data-help-text="数据位表示每个串口数据帧里真正承载数据的位数，常见为 8 位。某些老设备或特殊协议会使用 7 位甚至更少。">数据位</button>
            <select id="dataBits" name="dataBits">
              <option value="5">5</option>
              <option value="6">6</option>
              <option value="7">7</option>
              <option value="8">8</option>
            </select>
          </div>
          <div>
            <button type="button" class="label-button" data-help-title="校验位" data-help-text="校验位用于做简单的传输错误检测。N 表示无校验，E 表示偶校验，O 表示奇校验。两端设置必须一致。">校验位</button>
            <select id="parity" name="parity">
              <option value="N">N - 无校验</option>
              <option value="E">E - 偶校验</option>
              <option value="O">O - 奇校验</option>
            </select>
          </div>
          <div>
            <button type="button" class="label-button" data-help-title="停止位" data-help-text="停止位用于标记一个数据帧的结束。常见值是 1，某些设备会要求 2 个停止位以提升兼容性。">停止位</button>
            <select id="stopBits" name="stopBits">
              <option value="1">1</option>
              <option value="2">2</option>
            </select>
          </div>
        </div>
        <div style="margin-top: 16px;">
          <button type="submit">应用串口参数</button>
        </div>
      </form>
      <div class="status" id="status"></div>
      <div class="current" id="current">正在读取当前串口参数...</div>

      <div class="section">
        <h2>Wi-Fi 配置组</h2>
        <p class="subtle">最多保存 24 组 Wi-Fi 凭据，并选择设备当前使用哪一组。</p>
        <div class="current" id="wifi-current">正在读取 Wi-Fi 状态...</div>
        <form id="wifi-form" style="margin-top: 16px;">
          <div class="grid">
            <div>
              <label for="slotIndex">配置槽位</label>
              <select id="slotIndex" name="index"></select>
            </div>
            <div>
              <label for="wifiSsid">Wi-Fi 名称</label>
              <input id="wifiSsid" name="ssid" type="text" maxlength="32" required>
            </div>
            <div>
              <label for="wifiPassword">密码</label>
              <input id="wifiPassword" name="password" type="password" maxlength="64" placeholder="留空可保留当前槽位已有密码，或用于开放网络">
            </div>
            <div>
              <label for="activateProfile">保存后立即启用</label>
              <select id="activateProfile" name="activate">
                <option value="0">否</option>
                <option value="1">是</option>
              </select>
            </div>
          </div>
          <div style="margin-top: 16px;">
            <button type="submit">保存 Wi-Fi 配置</button>
          </div>
        </form>
        <div class="status" id="wifi-status"></div>
        <div class="subtle" id="wifi-password-hint">密码留空仅适用于开放网络，或保留当前槽位已保存的密码。</div>
        <div class="profiles" id="profiles">当前还没有保存任何 Wi-Fi 配置。</div>

        <div class="section">
          <h2>附近 Wi-Fi</h2>
          <p class="subtle">扫描附近热点时，网页控制台仍保持可访问。</p>
          <button type="button" id="scan-button">扫描附近 Wi-Fi</button>
          <div class="status" id="scan-status"></div>
          <div class="scan-list" id="scan-results">还没有扫描结果。</div>
        </div>
      </div>
    </section>
  </main>
  <div class="modal-backdrop" id="help-modal" aria-hidden="true">
    <div class="modal" role="dialog" aria-modal="true" aria-labelledby="help-modal-title">
      <h3 id="help-modal-title">说明</h3>
      <p id="help-modal-text"></p>
      <div class="modal-actions">
        <button type="button" id="help-modal-close">知道了</button>
      </div>
    </div>
  </div>
  <script>
    const form = document.getElementById('uart-form');
    const statusEl = document.getElementById('status');
    const currentEl = document.getElementById('current');
    const wifiForm = document.getElementById('wifi-form');
    const wifiStatusEl = document.getElementById('wifi-status');
    const wifiCurrentEl = document.getElementById('wifi-current');
    const wifiPasswordHintEl = document.getElementById('wifi-password-hint');
    const profilesEl = document.getElementById('profiles');
    const slotSelect = document.getElementById('slotIndex');
    const baudRatePresetEl = document.getElementById('baudRatePreset');
    const scanButton = document.getElementById('scan-button');
    const scanStatusEl = document.getElementById('scan-status');
    const scanResultsEl = document.getElementById('scan-results');
    const helpButtons = document.querySelectorAll('.label-button[data-help-title]');
    const helpModalEl = document.getElementById('help-modal');
    const helpModalTitleEl = document.getElementById('help-modal-title');
    const helpModalTextEl = document.getElementById('help-modal-text');
    const helpModalCloseEl = document.getElementById('help-modal-close');
    const commonBaudRates = ['9600', '19200', '38400', '57600', '115200', '230400', '460800', '921600'];

    for (let i = 0; i < 24; i += 1) {
      const option = document.createElement('option');
      option.value = String(i);
      option.textContent = `槽位 ${i}`;
      slotSelect.appendChild(option);
    }

    function renderCurrent(data) {
      currentEl.textContent =
        `波特率: ${data.baudRate} | 数据位: ${data.dataBits} | 校验位: ${data.parity} | 停止位: ${data.stopBits}`;
      form.baudRate.value = data.baudRate;
      baudRatePresetEl.value = commonBaudRates.includes(String(data.baudRate)) ? String(data.baudRate) : 'custom';
      form.dataBits.value = String(data.dataBits);
      form.parity.value = data.parity;
      form.stopBits.value = String(data.stopBits);
    }

    baudRatePresetEl.addEventListener('change', () => {
      if (baudRatePresetEl.value !== 'custom') {
        form.baudRate.value = baudRatePresetEl.value;
      }
    });

    form.baudRate.addEventListener('input', () => {
      const baudRate = String(form.baudRate.value);
      baudRatePresetEl.value = commonBaudRates.includes(baudRate) ? baudRate : 'custom';
    });

    function openHelpModal(title, text) {
      helpModalTitleEl.textContent = title;
      helpModalTextEl.textContent = text;
      helpModalEl.classList.add('open');
      helpModalEl.setAttribute('aria-hidden', 'false');
    }

    function closeHelpModal() {
      helpModalEl.classList.remove('open');
      helpModalEl.setAttribute('aria-hidden', 'true');
    }

    helpButtons.forEach((button) => {
      button.addEventListener('click', () => {
        const title = button.dataset.helpTitle || '说明';
        const text = button.dataset.helpText || '';
        openHelpModal(title, text);
      });
    });

    helpModalCloseEl.addEventListener('click', closeHelpModal);
    helpModalEl.addEventListener('click', (event) => {
      if (event.target === helpModalEl) {
        closeHelpModal();
      }
    });
    window.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && helpModalEl.classList.contains('open')) {
        closeHelpModal();
      }
    });

    async function refresh() {
      const response = await fetch('/api/uart');
      if (!response.ok) {
        throw new Error('读取当前串口参数失败');
      }
      const data = await response.json();
      renderCurrent(data);
    }

    function profileCard(profile) {
      const active = profile.active ? '当前启用' : '已保存';
      return `
        <div class="profile">
          <div class="profile-title">槽位 ${profile.index} - ${profile.ssid}</div>
          <div class="profile-meta">${active}</div>
          <div class="profile-actions">
            <button type="button" data-action="activate" data-index="${profile.index}">启用</button>
            <button type="button" data-action="delete" data-index="${profile.index}">删除</button>
          </div>
        </div>`;
    }

    function renderWiFi(data) {
      const modeText = data.apActive ? 'AP 模式' : (data.connected ? 'STA 模式' : '空闲');
      const activeText = data.activeIndex >= 0 ? `槽位 ${data.activeIndex}` : '无';
      const connectedText = data.connectedSsid || '无';
      const ipText = data.ip || '不可用';
      wifiCurrentEl.textContent =
        `当前模式: ${modeText} | 启用槽位: ${activeText} | 已连 Wi‑Fi: ${connectedText} | IP 地址: ${ipText}`;

      if (!data.profiles.length) {
        profilesEl.innerHTML = '<div class="subtle">当前还没有保存任何 Wi-Fi 配置。</div>';
      } else {
        profilesEl.innerHTML = data.profiles.map(profileCard).join('');
      }
    }

    function renderScanResults(data) {
      if (!data.networks.length) {
        scanResultsEl.innerHTML = '<div class="subtle">没有扫描到可见热点。</div>';
        return;
      }

      const sortedNetworks = [...data.networks].sort((left, right) => right.rssi - left.rssi);
      scanResultsEl.innerHTML = sortedNetworks.map((network) => `
        <div class="scan-item" data-ssid="${network.ssid}" data-open="${network.open ? '1' : '0'}">
          <div class="scan-title">${network.ssid || '<hidden>'}</div>
          <div class="scan-meta">信号: ${network.rssi} dBm | 信道: ${network.channel} | 安全: ${network.security}</div>
        </div>
      `).join('');
    }

    function updatePasswordHint(isOpenNetwork) {
      wifiPasswordHintEl.textContent = isOpenNetwork
        ? '已选择开放网络，可以不填写密码。'
        : '密码留空仅适用于开放网络，或保留当前槽位已保存的密码。';
    }

    async function refreshWiFi() {
      const response = await fetch('/api/wifi');
      if (!response.ok) {
        throw new Error('读取 Wi-Fi 配置失败');
      }
      const data = await response.json();
      renderWiFi(data);
    }

    async function postForm(url, params) {
      const response = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: params.toString(),
      });
      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.error || '请求失败');
      }
      await refreshWiFi();
      return data;
    }

    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      statusEl.textContent = '正在应用串口参数...';
      const params = new URLSearchParams(new FormData(form));
      try {
        const response = await fetch('/api/uart', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: params.toString(),
        });
        const data = await response.json();
        if (!response.ok) {
          throw new Error(data.error || '更新失败');
        }
        renderCurrent(data);
        statusEl.textContent = '串口参数已更新。';
      } catch (error) {
        statusEl.textContent = error.message;
      }
    });

    wifiForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      wifiStatusEl.textContent = '正在保存 Wi-Fi 配置...';
      try {
        await postForm('/api/wifi/save', new URLSearchParams(new FormData(wifiForm)));
        wifiStatusEl.textContent = 'Wi-Fi 配置已保存。';
      } catch (error) {
        wifiStatusEl.textContent = error.message;
      }
    });

    profilesEl.addEventListener('click', async (event) => {
      const button = event.target.closest('button[data-action]');
      if (!button) {
        return;
      }
      const index = button.dataset.index;
      const action = button.dataset.action;
      wifiStatusEl.textContent = `${action === 'activate' ? '正在启用' : '正在删除'} Wi-Fi 配置...`;
      try {
        const params = new URLSearchParams();
        params.set('index', index);
        await postForm(action === 'activate' ? '/api/wifi/activate' : '/api/wifi/delete', params);
        wifiStatusEl.textContent = action === 'activate' ? 'Wi-Fi 配置已启用。' : 'Wi-Fi 配置已删除。';
      } catch (error) {
        wifiStatusEl.textContent = error.message;
      }
    });

    scanResultsEl.addEventListener('click', (event) => {
      const item = event.target.closest('.scan-item[data-ssid]');
      if (!item) {
        return;
      }
      const ssid = item.dataset.ssid;
      const isOpenNetwork = item.dataset.open === '1';
      wifiForm.ssid.value = ssid;
      if (isOpenNetwork) {
        wifiForm.password.value = '';
      }
      wifiForm.activate.value = '1';
      updatePasswordHint(isOpenNetwork);
      wifiStatusEl.textContent = `已选择 ${ssid || '<hidden>'} 并填入 Wi-Fi 表单。`;
    });

    scanButton.addEventListener('click', async () => {
      scanStatusEl.textContent = '正在扫描附近 Wi-Fi...';
      try {
        const response = await fetch('/api/wifi/scan', { method: 'POST' });
        const data = await response.json();
        if (!response.ok) {
          throw new Error(data.error || '扫描失败');
        }
        renderScanResults(data);
        scanStatusEl.textContent = `共扫描到 ${data.networks.length} 个热点。`;
      } catch (error) {
        scanStatusEl.textContent = error.message;
      }
    });

    refresh().catch((error) => {
      statusEl.textContent = error.message;
      currentEl.textContent = '无法读取当前串口参数。';
    });

    refreshWiFi().catch((error) => {
      wifiStatusEl.textContent = error.message;
      wifiCurrentEl.textContent = '无法读取当前 Wi-Fi 状态。';
    });
    updatePasswordHint(false);
  </script>
</body>
</html>
)HTML";

class ByteRingBuffer {
 public:
  ByteRingBuffer() = default;

  ~ByteRingBuffer() {
    release();
  }

  ByteRingBuffer(const ByteRingBuffer &) = delete;
  ByteRingBuffer &operator=(const ByteRingBuffer &) = delete;
  ByteRingBuffer(ByteRingBuffer &&) = delete;
  ByteRingBuffer &operator=(ByteRingBuffer &&) = delete;

  [[nodiscard]]
  bool begin(size_t capacity) {
    release();

    if (capacity == 0) {
      return false;
    }

    data_ = static_cast<uint8_t *>(malloc(capacity));

    if (data_ == nullptr) {
      capacity_ = 0;
      ownsStorage_ = false;
      usingPsram_ = false;
      return false;
    }

    capacity_ = capacity;
    ownsStorage_ = true;
    clear();
    return true;
  }

  [[nodiscard]]
  bool begin(uint8_t *storage, size_t capacity) {
    release();

    if (storage == nullptr || capacity == 0) {
      return false;
    }

    data_ = storage;
    capacity_ = capacity;
    ownsStorage_ = false;
    usingPsram_ = false;
    clear();
    return true;
  }

  void release() {
    if (ownsStorage_ && data_ != nullptr) {
      free(data_);
    }

    data_ = nullptr;
    capacity_ = 0;
    ownsStorage_ = false;
    usingPsram_ = false;
    head_ = 0;
    tail_ = 0;
    count_ = 0;
  }

  [[nodiscard]] size_t capacity() const {
    return capacity_;
  }

  [[nodiscard]] bool usingPsram() const {
    return usingPsram_;
  }

  [[nodiscard]] size_t size() const {
    return count_;
  }

  [[nodiscard]] size_t freeSpace() const {
    return capacity_ - count_;
  }

  size_t push(const uint8_t *data, size_t length) {
    if (data_ == nullptr || capacity_ == 0) {
      return 0;
    }

    size_t written = 0;
    while (written < length && count_ < capacity_) {
      data_[head_] = data[written++];
      head_ = (head_ + 1) % capacity_;
      ++count_;
    }
    return written;
  }

  size_t peek(uint8_t *data, size_t length) const {
    if (data_ == nullptr || capacity_ == 0) {
      return 0;
    }

    const size_t available = min(length, count_);
    size_t index = tail_;
    for (size_t i = 0; i < available; ++i) {
      data[i] = data_[index];
      index = (index + 1) % capacity_;
    }
    return available;
  }

  void discard(size_t length) {
    if (capacity_ == 0) {
      return;
    }

    const size_t discarded = min(length, count_);
    tail_ = (tail_ + discarded) % capacity_;
    count_ -= discarded;
  }

  void clear() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
  }

 private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
  bool ownsStorage_ = false;
  bool usingPsram_ = false;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
};

HardwareSerial UartPort(1);
WiFiServer TcpServer(TCP_BRIDGE_PORT);
WiFiClient TcpClient;
WebServer HttpServer(kHttpPort);
Preferences PreferencesStore;
uint8_t gTcpToUartStorage[kPendingTcpToUartBytes] = {};
uint8_t gUartToTcpStorage[kPendingUartToTcpBytes] = {};
ByteRingBuffer gTcpToUartBuffer;
ByteRingBuffer gUartToTcpBuffer;
UartSettings gUartSettings = {kUartBaudRate, 8, 'N', 1};
WiFiProfile gWiFiProfiles[kMaxWiFiProfiles] = {};
uint32_t gLastActivityAtMs = 0;
uint32_t gLastWifiReconnectAttemptMs = 0;
uint32_t gLastUartBackpressureLogAtMs = 0;
uint32_t gLastStatsLogAtMs = 0;
bool gUseStationMode = false;
bool gAccessPointActive = false;
bool gTcpServerStarted = false;
bool gStationWasConnected = false;
bool gTcpClientActive = false;
bool gWifiScanInProgress = false;
bool gPendingWiFiReconfigure = false;
int8_t gActiveWiFiProfileIndex = -1;
uint64_t gUartToTcpBytes = 0;
uint64_t gTcpToUartBytes = 0;
uint32_t gTcpClientConnectCount = 0;
uint32_t gTcpClientDisconnectCount = 0;
uint32_t gWifiReconnectCount = 0;
uint32_t gUartBackpressureEvents = 0;
uint32_t gUartToTcpOverflowEvents = 0;
uint32_t gTcpToUartOverflowEvents = 0;
uint32_t gTcpPartialWriteEvents = 0;
WiFiScanResult gWiFiScanResults[kMaxWiFiProfiles] = {};
size_t gWiFiScanResultCount = 0;

void clearSessionBuffers();
void showStatusLed();

void writeStatusLed(bool on) {
  const int activeLevel = STATUS_LED_ACTIVE_LEVEL ? HIGH : LOW;
  const int inactiveLevel = STATUS_LED_ACTIVE_LEVEL ? LOW : HIGH;
  digitalWrite(STATUS_LED_PIN, on ? activeLevel : inactiveLevel);
}

void sendJsonDocument(int statusCode, JsonDocument &doc) {
  String response;
  serializeJson(doc, response);
  HttpServer.send(statusCode, "application/json", response);
}

void sendJsonError(int statusCode, const String &message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJsonDocument(statusCode, doc);
}

void makeWiFiProfileKeys(uint8_t index, char *ssidKey, char *passwordKey) {
  snprintf(ssidKey, 4, "s%02u", index);
  snprintf(passwordKey, 4, "p%02u", index);
}

int countStoredWiFiProfiles() {
  int count = 0;
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    if (gWiFiProfiles[i].inUse) {
      ++count;
    }
  }
  return count;
}

const WiFiProfile *getActiveWiFiProfile() {
  if (gActiveWiFiProfileIndex < 0 || gActiveWiFiProfileIndex >= kMaxWiFiProfiles) {
    return nullptr;
  }

  return gWiFiProfiles[gActiveWiFiProfileIndex].inUse ? &gWiFiProfiles[gActiveWiFiProfileIndex] : nullptr;
}

void persistActiveWiFiProfileIndex() {
  PreferencesStore.putUChar("aidx", gActiveWiFiProfileIndex >= 0 ? static_cast<uint8_t>(gActiveWiFiProfileIndex) : 255);
}

bool setActiveWiFiProfileIndex(int8_t index) {
  if (index < 0) {
    gActiveWiFiProfileIndex = -1;
    persistActiveWiFiProfileIndex();
    return true;
  }

  if (index >= kMaxWiFiProfiles || !gWiFiProfiles[index].inUse) {
    return false;
  }

  gActiveWiFiProfileIndex = index;
  persistActiveWiFiProfileIndex();
  return true;
}

void loadWiFiProfiles() {
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    char ssidKey[4] = {0};
    char passwordKey[4] = {0};
    makeWiFiProfileKeys(i, ssidKey, passwordKey);
    const String ssid = PreferencesStore.isKey(ssidKey) ? PreferencesStore.getString(ssidKey, String()) : String();
    const String password = PreferencesStore.isKey(passwordKey) ? PreferencesStore.getString(passwordKey, String()) : String();
    gWiFiProfiles[i] = {!ssid.isEmpty(), ssid, password};
  }

  const uint8_t activeIndex = PreferencesStore.getUChar("aidx", 255);
  if (activeIndex < kMaxWiFiProfiles && gWiFiProfiles[activeIndex].inUse) {
    gActiveWiFiProfileIndex = static_cast<int8_t>(activeIndex);
  } else {
    gActiveWiFiProfileIndex = -1;
  }

  debugPrintf(
      "Loaded Wi-Fi profiles: count=%d active=%d\n",
      countStoredWiFiProfiles(),
      gActiveWiFiProfileIndex);
  if (gActiveWiFiProfileIndex >= 0) {
    debugPrintf(
        "Active Wi-Fi profile SSID: %s\n",
        gWiFiProfiles[gActiveWiFiProfileIndex].ssid.c_str());
  }
}

bool saveWiFiProfile(uint8_t index, const String &ssid, const String &password) {
  if (index >= kMaxWiFiProfiles || ssid.isEmpty()) {
    return false;
  }

  char ssidKey[4] = {0};
  char passwordKey[4] = {0};
  makeWiFiProfileKeys(index, ssidKey, passwordKey);

  if (PreferencesStore.putString(ssidKey, ssid) == 0) {
    return false;
  }

  PreferencesStore.putString(passwordKey, password);
  gWiFiProfiles[index] = {true, ssid, password};
  return true;
}

void deleteWiFiProfile(uint8_t index) {
  if (index >= kMaxWiFiProfiles) {
    return;
  }

  char ssidKey[4] = {0};
  char passwordKey[4] = {0};
  makeWiFiProfileKeys(index, ssidKey, passwordKey);
  PreferencesStore.remove(ssidKey);
  PreferencesStore.remove(passwordKey);
  gWiFiProfiles[index] = {false, String(), String()};

  if (gActiveWiFiProfileIndex == static_cast<int8_t>(index)) {
    setActiveWiFiProfileIndex(-1);
  }
}

/**
 * @brief Seed profile slot 0 from compile-time macros when storage is empty.
 * @note Seeding is skipped if any profile already exists or WIFI_SSID is empty.
 */
void seedDefaultWiFiProfileIfNeeded() {
  if (countStoredWiFiProfiles() > 0 || strlen(WIFI_SSID) == 0) {
    debugPrintf(
        "Skipping build-flag Wi-Fi seed: profiles=%d, wifi_ssid_empty=%s\n",
        countStoredWiFiProfiles(),
        strlen(WIFI_SSID) == 0 ? "yes" : "no");
    return;
  }

  if (saveWiFiProfile(0, WIFI_SSID, WIFI_PASSWORD)) {
    setActiveWiFiProfileIndex(0);
    debugPrintln("Seeded Wi-Fi profile slot 0 from build flags");
  }
}

bool initPreferences() {
  if (!PreferencesStore.begin("bridgecfg", false)) {
    return false;
  }

  loadWiFiProfiles();
  seedDefaultWiFiProfileIfNeeded();
  loadWiFiProfiles();
  return true;
}

void addWiFiProfilesJson(JsonDocument &doc) {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const String ip = gAccessPointActive ? WiFi.softAPIP().toString() : (wifiConnected ? WiFi.localIP().toString() : String());
  doc["activeIndex"] = gActiveWiFiProfileIndex;
  doc["connected"] = wifiConnected;
  doc["apActive"] = gAccessPointActive;
  doc["connectedSsid"] = wifiConnected ? WiFi.SSID() : String();
  doc["ip"] = ip;
  JsonArray profiles = doc["profiles"].to<JsonArray>();

  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    if (!gWiFiProfiles[i].inUse) {
      continue;
    }

    JsonObject profile = profiles.add<JsonObject>();
    profile["index"] = i;
    profile["ssid"] = gWiFiProfiles[i].ssid;
    profile["active"] = gActiveWiFiProfileIndex == static_cast<int8_t>(i);
  }
}

void queueWiFiReconfigure() {
  gPendingWiFiReconfigure = true;
}

String wifiSecurityLabel(uint8_t encryptionType) {
  switch (encryptionType) {
    case WIFI_AUTH_OPEN:
      return "Open";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI";
    default:
      return "Unknown";
  }
}

void addWiFiScanResultsJson(JsonDocument &doc) {
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (size_t i = 0; i < gWiFiScanResultCount; ++i) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = gWiFiScanResults[i].ssid;
    network["rssi"] = gWiFiScanResults[i].rssi;
    network["channel"] = gWiFiScanResults[i].channel;
    network["security"] = wifiSecurityLabel(gWiFiScanResults[i].encryption);
    network["open"] = gWiFiScanResults[i].encryption == WIFI_AUTH_OPEN;
  }
}

void scanNearbyWiFi() {
  gWifiScanInProgress = true;
  showStatusLed();

  if (gAccessPointActive) {
    WiFi.mode(WIFI_AP_STA);
  }

  gWiFiScanResultCount = 0;
  int16_t count = WiFi.scanNetworks(true, true);
  while (count == WIFI_SCAN_RUNNING) {
    delay(30);
    showStatusLed();
    count = WiFi.scanComplete();
  }
  if (count <= 0) {
    WiFi.scanDelete();
    if (gAccessPointActive) {
      WiFi.mode(WIFI_AP);
    }
    gWifiScanInProgress = false;
    showStatusLed();
    return;
  }

  const size_t cappedCount = min(static_cast<size_t>(count), static_cast<size_t>(kMaxWiFiProfiles));
  for (size_t i = 0; i < cappedCount; ++i) {
    gWiFiScanResults[i].ssid = WiFi.SSID(i);
    gWiFiScanResults[i].rssi = WiFi.RSSI(i);
    gWiFiScanResults[i].channel = static_cast<uint8_t>(WiFi.channel(i));
    gWiFiScanResults[i].encryption = static_cast<uint8_t>(WiFi.encryptionType(i));
  }
  gWiFiScanResultCount = cappedCount;
  WiFi.scanDelete();

  if (gAccessPointActive) {
    WiFi.mode(WIFI_AP);
  }

  gWifiScanInProgress = false;
  showStatusLed();
}

bool isTcpClientConnected() {
  return gTcpClientActive && TcpClient.connected();
}

/**
 * @brief Convert user UART settings into Arduino serial config flags.
 * @param settings Requested UART parameters.
 * @param[out] serialConfig Arduino serial framing constant.
 * @retval true Combination is supported.
 * @retval false Combination is invalid or unsupported.
 */
bool mapUartConfig(const UartSettings &settings, uint32_t &serialConfig) {
  if (settings.stopBits != 1 && settings.stopBits != 2) {
    return false;
  }

  switch (settings.dataBits) {
    case 5:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_5N1 : SERIAL_5N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_5E1 : SERIAL_5E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_5O1 : SERIAL_5O2;
        return true;
      }
      return false;
    case 6:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_6N1 : SERIAL_6N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_6E1 : SERIAL_6E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_6O1 : SERIAL_6O2;
        return true;
      }
      return false;
    case 7:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_7N1 : SERIAL_7N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_7E1 : SERIAL_7E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_7O1 : SERIAL_7O2;
        return true;
      }
      return false;
    case 8:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_8N1 : SERIAL_8N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_8E1 : SERIAL_8E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_8O1 : SERIAL_8O2;
        return true;
      }
      return false;
    default:
      return false;
  }
}

bool applyUartSettings(const UartSettings &settings) {
  uint32_t serialConfig = SERIAL_8N1;
  if (!mapUartConfig(settings, serialConfig)) {
    return false;
  }

  clearSessionBuffers();
  UartPort.flush();
  UartPort.end();
  UartPort.setRxBufferSize(kUartDriverRxBufferSize);
  UartPort.setTxBufferSize(kUartDriverTxBufferSize);
  UartPort.begin(
      settings.baudRate,
      serialConfig,
      UART1_RX_PIN,
      UART1_TX_PIN,
      false,
      20000UL,
      kUartRxFifoFullThreshold);
  UartPort.setTimeout(0);
  gUartSettings = settings;

  debugPrintf(
      "UART1 reconfigured. Baud=%" PRIu32 ", Data=%u, Parity=%c, Stop=%u\n",
      gUartSettings.baudRate,
      gUartSettings.dataBits,
      gUartSettings.parity,
      gUartSettings.stopBits);
  return true;
}

void addUartSettingsJson(JsonDocument &doc) {
  doc["baudRate"] = gUartSettings.baudRate;
  doc["dataBits"] = gUartSettings.dataBits;
  char parity[2] = {gUartSettings.parity, '\0'};
  doc["parity"] = parity;
  doc["stopBits"] = gUartSettings.stopBits;
}

/**
 * @brief Parse and validate UART settings from HTTP request arguments.
 * @param[out] settings Parsed UART settings when validation succeeds.
 * @param[out] error Human-readable validation error when parsing fails.
 * @retval true Request contains a supported UART framing combination.
 * @retval false Request is missing fields or contains unsupported values.
 */
bool parseUartSettingsFromRequest(UartSettings &settings, String &error) {
  if (!HttpServer.hasArg("baudRate") || !HttpServer.hasArg("dataBits") ||
      !HttpServer.hasArg("parity") || !HttpServer.hasArg("stopBits")) {
    error = "Missing required UART parameters";
    return false;
  }

  const uint32_t baudRate = static_cast<uint32_t>(HttpServer.arg("baudRate").toInt());
  const uint8_t dataBits = static_cast<uint8_t>(HttpServer.arg("dataBits").toInt());
  const String parityArg = HttpServer.arg("parity");
  const uint8_t stopBits = static_cast<uint8_t>(HttpServer.arg("stopBits").toInt());

  if (baudRate < 300) {
    error = "Baud rate must be >= 300";
    return false;
  }

  if (parityArg.length() != 1) {
    error = "Parity must be one of N, E, or O";
    return false;
  }

  settings = {baudRate, dataBits, static_cast<char>(toupper(parityArg[0])), stopBits};
  uint32_t serialConfig = SERIAL_8N1;
  if (!mapUartConfig(settings, serialConfig)) {
    error = "Unsupported UART framing combination";
    return false;
  }

  return true;
}

/**
 * @brief Serve the embedded web configuration page.
 * @route GET /
 */
void handleConfigPage() {
  HttpServer.send(200, "text/html; charset=utf-8", FPSTR(kConfigPageHtml));
}

/**
 * @brief Return current Wi-Fi profile list and network status as JSON.
 * @route GET /api/wifi
 */
void handleGetWiFiProfiles() {
  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief Return current UART runtime settings as JSON.
 * @route GET /api/uart
 */
void handleGetUartSettings() {
  JsonDocument doc;
  addUartSettingsJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief Apply UART settings from HTTP form arguments and return updated state.
 * @route POST /api/uart
 */
void handleSetUartSettings() {
  UartSettings requested = gUartSettings;
  String error;
  if (!parseUartSettingsFromRequest(requested, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!applyUartSettings(requested)) {
    sendJsonError(400, "Failed to apply UART settings");
    return;
  }

  JsonDocument doc;
  addUartSettingsJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief Parse Wi-Fi profile index argument from HTTP request.
 * @param[out] index Parsed profile slot index.
 * @param[out] error Validation error message on failure.
 * @retval true Index exists and is within [0, kMaxWiFiProfiles).
 * @retval false Index is missing or out of range.
 */
bool parseProfileIndexArg(uint8_t &index, String &error) {
  if (!HttpServer.hasArg("index")) {
    error = "Missing profile index";
    return false;
  }

  const int parsed = HttpServer.arg("index").toInt();
  if (parsed < 0 || parsed >= kMaxWiFiProfiles) {
    error = "Profile index out of range";
    return false;
  }

  index = static_cast<uint8_t>(parsed);
  return true;
}

/**
 * @brief Save a Wi-Fi profile and optionally activate it.
 * @details Expects `index` and `ssid`; `password` may be empty to keep existing value.
 * @route POST /api/wifi/save
 */
void handleSaveWiFiProfile() {
  uint8_t index = 0;
  String error;
  if (!parseProfileIndexArg(index, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!HttpServer.hasArg("ssid")) {
    sendJsonError(400, "Missing Wi-Fi name");
    return;
  }

  const String ssid = HttpServer.arg("ssid");
  if (ssid.isEmpty()) {
    sendJsonError(400, "Wi-Fi name cannot be empty");
    return;
  }

  String password = HttpServer.arg("password");
  if (password.isEmpty() && gWiFiProfiles[index].inUse) {
    password = gWiFiProfiles[index].password;
  }

  if (!saveWiFiProfile(index, ssid, password)) {
    sendJsonError(500, "Failed to save Wi-Fi profile");
    return;
  }

  if (HttpServer.arg("activate") == "1") {
    setActiveWiFiProfileIndex(static_cast<int8_t>(index));
    queueWiFiReconfigure();
  }

  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief Mark an existing Wi-Fi profile as active and trigger reconfiguration.
 * @route POST /api/wifi/activate
 */
void handleActivateWiFiProfile() {
  uint8_t index = 0;
  String error;
  if (!parseProfileIndexArg(index, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!gWiFiProfiles[index].inUse || !setActiveWiFiProfileIndex(static_cast<int8_t>(index))) {
    sendJsonError(400, "Selected profile does not exist");
    return;
  }

  queueWiFiReconfigure();
  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief Delete a Wi-Fi profile slot.
 * @details Deleting the active slot schedules a Wi-Fi mode/profile reconfiguration.
 * @route POST /api/wifi/delete
 */
void handleDeleteWiFiProfile() {
  uint8_t index = 0;
  String error;
  if (!parseProfileIndexArg(index, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!gWiFiProfiles[index].inUse) {
    sendJsonError(400, "Selected profile does not exist");
    return;
  }

  const bool deletedActiveProfile = gActiveWiFiProfileIndex == static_cast<int8_t>(index);
  deleteWiFiProfile(index);
  if (deletedActiveProfile) {
    queueWiFiReconfigure();
  }

  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief Perform a nearby Wi-Fi scan and return results as JSON.
 * @route POST /api/wifi/scan
 */
void handleScanWiFi() {
  scanNearbyWiFi();
  JsonDocument doc;
  addWiFiScanResultsJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief Default handler for unknown HTTP routes.
 */
void handleNotFound() {
  sendJsonError(404, "Not found");
}

/**
 * @brief Register HTTP routes and start the web configuration server.
 */
void initHttpServer() {
  HttpServer.on("/", HTTP_GET, handleConfigPage);
  HttpServer.on("/api/wifi", HTTP_GET, handleGetWiFiProfiles);
  HttpServer.on("/api/wifi/scan", HTTP_POST, handleScanWiFi);
  HttpServer.on("/api/wifi/save", HTTP_POST, handleSaveWiFiProfile);
  HttpServer.on("/api/wifi/activate", HTTP_POST, handleActivateWiFiProfile);
  HttpServer.on("/api/wifi/delete", HTTP_POST, handleDeleteWiFiProfile);
  HttpServer.on("/api/uart", HTTP_GET, handleGetUartSettings);
  HttpServer.on("/api/uart", HTTP_POST, handleSetUartSettings);
  HttpServer.onNotFound(handleNotFound);
  HttpServer.begin();
  debugPrintf("HTTP config page listening on port %u\n", kHttpPort);
}

/**
 * @brief Initialize bridge ring buffers for both data directions.
 * @details The C3 build binds both ring buffers to fixed static storage for long-run determinism.
 * @retval true Both buffers were allocated successfully.
 * @retval false At least one allocation failed.
 */
bool initBridgeBuffers() {
  const bool tcpToUartReady = gTcpToUartBuffer.begin(gTcpToUartStorage, sizeof(gTcpToUartStorage));
  const bool uartToTcpReady = gUartToTcpBuffer.begin(gUartToTcpStorage, sizeof(gUartToTcpStorage));

  if (!tcpToUartReady || !uartToTcpReady) {
    gTcpToUartBuffer.release();
    gUartToTcpBuffer.release();
    return false;
  }

  debugPrintf(
      "Bridge buffers ready. TCP->UART=%" PRIu32 " (%s), UART->TCP=%" PRIu32 " (%s)\n",
      static_cast<uint32_t>(gTcpToUartBuffer.capacity()),
      gTcpToUartBuffer.usingPsram() ? "PSRAM" : "SRAM",
      static_cast<uint32_t>(gUartToTcpBuffer.capacity()),
      gUartToTcpBuffer.usingPsram() ? "PSRAM" : "SRAM");
  return true;
}

bool isNetworkReady() {
  return gAccessPointActive || WiFi.status() == WL_CONNECTED;
}

void markActivity() {
  gLastActivityAtMs = millis();
}

void showStatusLed() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool tcpClientConnected = isTcpClientConnected();
  const uint32_t activityAgeMs = millis() - gLastActivityAtMs;
  const bool inDataFlashWindow = activityAgeMs <= kDataFlashWindowMs;

  bool ledOn = false;
  if (inDataFlashWindow) {
    ledOn = (millis() / 40) % 2 == 0;
  } else if (tcpClientConnected) {
    ledOn = true;
  } else if (wifiConnected) {
    ledOn = (millis() / kWifiBlinkPeriodMs) % 2 == 0;
  } else {
    ledOn = (millis() / kDisconnectedBlinkPeriodMs) % 2 == 0;
  }

  writeStatusLed(ledOn);
}

void stopTcpServer() {
  if (!gTcpServerStarted) {
    return;
  }

  TcpServer.end();
  gTcpServerStarted = false;
}

void clearSessionBuffers() {
  gTcpToUartBuffer.clear();
  gUartToTcpBuffer.clear();
}

void disconnectTcpClient(const char *reason) {
  const bool hadSession = gTcpClientActive;
  TcpClient.stop();
  gTcpClientActive = false;
  if (hadSession) {
    ++gTcpClientDisconnectCount;
  }
  debugPrintf("TCP client disconnected: %s\n", reason);
}

void startTcpServer() {
  if (gTcpServerStarted || !isNetworkReady()) {
    return;
  }

  TcpServer.begin();
  TcpServer.setNoDelay(true);
  gTcpServerStarted = true;
  debugPrintf("TCP bridge listening on port %d\n", TCP_BRIDGE_PORT);
}

void logStationReady() {
  debugPrintf(
      "STA mode ready. IP: %s, TCP port: %d\n",
      WiFi.localIP().toString().c_str(),
      TCP_BRIDGE_PORT);
}

void startAccessPoint() {
  gUseStationMode = false;
  gAccessPointActive = false;
  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    debugPrintln("Failed to start WiFi AP");
    return;
  }

  gAccessPointActive = true;
  debugPrintf(
      "AP mode ready. SSID: %s, password: %s, IP: %s, TCP port: %d\n",
      AP_SSID,
      AP_PASSWORD,
      WiFi.softAPIP().toString().c_str(),
      TCP_BRIDGE_PORT);
}

void stopStationBeforeAccessPoint() {
  gUseStationMode = false;
  gStationWasConnected = false;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_MODE_NULL);
  delay(100);
}

/**
 * @brief Switch Wi-Fi to STA mode and start connecting with active profile.
 * @note Existing TCP session is dropped because network context changes.
 */
void startStationMode() {
  const WiFiProfile *activeProfile = getActiveWiFiProfile();
  if (activeProfile == nullptr) {
    debugPrintln("No active Wi-Fi profile, unable to enter STA mode");
    return;
  }

  gUseStationMode = true;
  gAccessPointActive = false;
  gStationWasConnected = false;
  stopTcpServer();
  if (gTcpClientActive) {
    disconnectTcpClient("wifi profile change");
  }
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(activeProfile->ssid.c_str(), activeProfile->password.c_str());
  gLastWifiReconnectAttemptMs = millis();
  debugPrintf("Connecting to WiFi SSID: %s\n", activeProfile->ssid.c_str());
}

/**
 * @brief Block during boot for initial STA connection attempt.
 * @details If timeout expires, firmware falls back to AP mode and starts TCP service there.
 */
void waitForInitialStationConnection() {
  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < kWifiConnectTimeoutMs) {
    delay(250);
    debugPrint(".");
    showStatusLed();
  }
  debugPrintln("");

  if (WiFi.status() == WL_CONNECTED) {
    gStationWasConnected = true;
    ++gWifiReconnectCount;
    logStationReady();
    startTcpServer();
  } else {
    debugPrintln("Initial WiFi connect timed out, falling back to AP mode");
    stopStationBeforeAccessPoint();
    startAccessPoint();
    startTcpServer();
  }
}

void handleStationMode() {
  if (!gUseStationMode) {
    return;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !gStationWasConnected) {
    gStationWasConnected = true;
    ++gWifiReconnectCount;
    logStationReady();
    startTcpServer();
  }

  if (!connected && gStationWasConnected) {
    gStationWasConnected = false;
    debugPrintln("WiFi disconnected");
    stopTcpServer();
    if (gTcpClientActive) {
      disconnectTcpClient("wifi lost");
    }
  }

  if (!connected && millis() - gLastWifiReconnectAttemptMs >= kWifiReconnectIntervalMs) {
    debugPrintln("Retrying WiFi connection");
    if (!WiFi.reconnect()) {
      const WiFiProfile *activeProfile = getActiveWiFiProfile();
      if (activeProfile != nullptr) {
        WiFi.disconnect(false, false);
        WiFi.begin(activeProfile->ssid.c_str(), activeProfile->password.c_str());
      }
    }
    gLastWifiReconnectAttemptMs = millis();
  }
}

/**
 * @brief Apply deferred Wi-Fi mode/profile changes requested by HTTP handlers.
 * @details Runs in loop context to keep request callbacks short and side effects centralized.
 */
void applyPendingWiFiReconfigureIfNeeded() {
  if (!gPendingWiFiReconfigure) {
    return;
  }

  gPendingWiFiReconfigure = false;
  if (getActiveWiFiProfile() != nullptr) {
    startStationMode();
    waitForInitialStationConnection();
    return;
  }

  stopTcpServer();
  if (gTcpClientActive) {
    disconnectTcpClient("wifi profile removed");
  }
  stopStationBeforeAccessPoint();
  startAccessPoint();
  startTcpServer();
}

void acceptClientIfNeeded() {
  if (!gTcpServerStarted) {
    return;
  }

  if (gTcpClientActive && !TcpClient.connected()) {
    disconnectTcpClient("peer closed");
  }

  if (isTcpClientConnected()) {
    return;
  }

  WiFiClient newClient = TcpServer.accept();
  if (!newClient) {
    return;
  }

  newClient.setNoDelay(true);
  TcpClient = newClient;
  gTcpClientActive = true;
  ++gTcpClientConnectCount;
  debugPrintf("TCP client connected: %s\n", TcpClient.remoteIP().toString().c_str());
}

/**
 * @brief Drain incoming TCP bytes into the TCP->UART ring buffer.
 * @note Overflow events are counted when socket input outruns UART consumption.
 */
void pullTcpIntoBuffer() {
  if (!isTcpClientConnected()) {
    return;
  }

  uint8_t buffer[kIoChunkSize];
  while (TcpClient.available() > 0 && gTcpToUartBuffer.freeSpace() > 0) {
    const size_t requestSize = min(
        static_cast<size_t>(TcpClient.available()),
        min(sizeof(buffer), gTcpToUartBuffer.freeSpace()));
    const int readSize = TcpClient.read(buffer, requestSize);
    if (readSize <= 0) {
      break;
    }

    gTcpToUartBuffer.push(buffer, static_cast<size_t>(readSize));
    markActivity();
  }

  if (TcpClient.available() > 0 && gTcpToUartBuffer.freeSpace() == 0) {
    ++gTcpToUartOverflowEvents;
  }
}

/**
 * @brief Flush buffered TCP payload to UART while respecting TX headroom.
 */
void flushTcpBufferToUart() {
  if (gTcpToUartBuffer.size() == 0) {
    return;
  }

  const int uartWriteSpace = UartPort.availableForWrite();
  if (uartWriteSpace <= 0) {
    return;
  }

  uint8_t buffer[kIoChunkSize];
  while (gTcpToUartBuffer.size() > 0) {
    const int availableSpace = UartPort.availableForWrite();
    if (availableSpace <= 0) {
      break;
    }

    const size_t chunkSize = min(
        gTcpToUartBuffer.size(),
        min(sizeof(buffer), static_cast<size_t>(availableSpace)));
    gTcpToUartBuffer.peek(buffer, chunkSize);
    const size_t written = UartPort.write(buffer, chunkSize);
    if (written == 0) {
      break;
    }

    gTcpToUartBuffer.discard(written);
    gTcpToUartBytes += written;
    markActivity();
  }
}

/**
 * @brief Read UART RX bytes into the UART->TCP ring buffer.
 * @warning Continuous sender pressure can drop bytes when buffer remains full.
 */
void pullUartIntoBuffer() {
  uint8_t buffer[kIoChunkSize];
  while (UartPort.available() > 0 && gUartToTcpBuffer.freeSpace() > 0) {
    const size_t requestSize = min(
        static_cast<size_t>(UartPort.available()),
        min(sizeof(buffer), gUartToTcpBuffer.freeSpace()));
    const size_t readSize = UartPort.readBytes(buffer, requestSize);
    if (readSize == 0) {
      break;
    }

    gUartToTcpBuffer.push(buffer, readSize);
    markActivity();
  }

  if (UartPort.available() > 0 && gUartToTcpBuffer.freeSpace() == 0 &&
      millis() - gLastUartBackpressureLogAtMs >= kUartBackpressureLogIntervalMs) {
    ++gUartBackpressureEvents;
    ++gUartToTcpOverflowEvents;
    gLastUartBackpressureLogAtMs = millis();
    debugPrintf(
        "Warning: UART backlog full (%" PRIu32 " bytes pending). Data loss is possible if the sender keeps streaming.\n",
        static_cast<uint32_t>(kPendingUartToTcpBytes));
  }
}

/**
 * @brief Flush buffered UART payload to the active TCP client.
 * @note Partial TCP writes are tracked for throughput diagnostics.
 */
void flushUartBufferToTcp() {
  if (!isTcpClientConnected() || gUartToTcpBuffer.size() == 0) {
    return;
  }

  uint8_t buffer[kIoChunkSize];
  while (isTcpClientConnected() && gUartToTcpBuffer.size() > 0) {
    if (!TcpClient.connected()) {
      disconnectTcpClient("peer reset before write");
      break;
    }

    const size_t chunkSize = min(gUartToTcpBuffer.size(), sizeof(buffer));
    gUartToTcpBuffer.peek(buffer, chunkSize);
    const size_t written = TcpClient.write(buffer, chunkSize);
    if (written == 0) {
      if (!TcpClient.connected()) {
        disconnectTcpClient("peer reset during write");
      }
      break;
    }

    if (written < chunkSize) {
      ++gTcpPartialWriteEvents;
    }

    gUartToTcpBuffer.discard(written);
    gUartToTcpBytes += written;
    markActivity();
  }
}

void logBridgeStatsIfNeeded() {
  if (millis() - gLastStatsLogAtMs < kStatsLogIntervalMs) {
    return;
  }

  gLastStatsLogAtMs = millis();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const String ip = gAccessPointActive ? WiFi.softAPIP().toString() : (wifiConnected ? WiFi.localIP().toString() : String("-"));
  const String ssid = gAccessPointActive ? String(AP_SSID) : (wifiConnected ? WiFi.SSID() : String("-"));
  debugPrintf(
      "Stats: wifi=%s ap=%s ssid=%s ip=%s tcp=%s uart->tcp=%" PRIu64 " tcp->uart=%" PRIu64 " pending(u2t/t2u)=%" PRIu32 "/%" PRIu32 " tcp_conn=%" PRIu32 " tcp_disc=%" PRIu32 " wifi_reconn=%" PRIu32 " uart_backpressure=%" PRIu32 " uart_overflow=%" PRIu32 " tcp_overflow=%" PRIu32 " tcp_partial=%" PRIu32 "\n",
      wifiConnected ? "up" : "down",
      gAccessPointActive ? "on" : "off",
      ssid.c_str(),
      ip.c_str(),
      isTcpClientConnected() ? "up" : "down",
      static_cast<uint64_t>(gUartToTcpBytes),
      static_cast<uint64_t>(gTcpToUartBytes),
      static_cast<uint32_t>(gUartToTcpBuffer.size()),
      static_cast<uint32_t>(gTcpToUartBuffer.size()),
      gTcpClientConnectCount,
      gTcpClientDisconnectCount,
      gWifiReconnectCount,
      gUartBackpressureEvents,
      gUartToTcpOverflowEvents,
      gTcpToUartOverflowEvents,
      gTcpPartialWriteEvents);
}

}  // namespace

void setup() {
  if constexpr (kEnableDebugLogs) {
    Serial.begin(kDebugBaudRate);
  }
  if (!initPreferences()) {
    debugPrintln("Failed to initialize Preferences. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  if (!initBridgeBuffers()) {
    debugPrintln("Failed to allocate bridge buffers. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  if (!applyUartSettings(gUartSettings)) {
    debugPrintln("Failed to initialize UART1. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  showStatusLed();

  WiFi.setSleep(false);
  WiFi.setTxPower(kWifiTxPower);
  if (getActiveWiFiProfile() != nullptr) {
    startStationMode();
    waitForInitialStationConnection();
  } else {
    startAccessPoint();
    startTcpServer();
  }
  initHttpServer();
  gLastStatsLogAtMs = millis();

  debugPrintf(
      "UART1 bridge ready. RX=%d, TX=%d, Baud=%" PRIu32 ", %u%c%u\n",
      UART1_RX_PIN,
      UART1_TX_PIN,
      gUartSettings.baudRate,
      gUartSettings.dataBits,
      gUartSettings.parity,
      gUartSettings.stopBits);
}

void loop() {
  handleStationMode();
  if (!gUseStationMode) {
    startTcpServer();
  }

  HttpServer.handleClient();
  applyPendingWiFiReconfigureIfNeeded();
  acceptClientIfNeeded();
  pullTcpIntoBuffer();
  flushTcpBufferToUart();
  pullUartIntoBuffer();
  flushUartBufferToTcp();
  logBridgeStatsIfNeeded();
  showStatusLed();
  delay(1);
}
