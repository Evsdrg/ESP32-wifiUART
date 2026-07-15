/**
 * @file   web_page.cpp
 * @brief  配置网页内容（嵌入式 HTML/CSS/JavaScript）
 *
 * 页面包含：UART 参数配置表单、Wi-Fi Profile 管理、附近热点扫描。
 * SSID 输出使用 escapeHtml() 转义，防止 XSS 注入。
 */

#include "web_page.h"

namespace wifi_uart {

const char kConfigPageHtml[] PROGMEM = R"HTML(
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
              <label for="keepPassword">密码留空时</label>
              <select id="keepPassword" name="keepPassword">
                <option value="1">保留该槽位旧密码</option>
                <option value="0">保存为空密码</option>
              </select>
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
        <div class="subtle" id="wifi-password-hint">密码留空默认保留旧密码；如果要开放网络或清空密码，请选择保存为空密码。</div>
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
    // DOM 元素缓存，避免重复查询
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
    let scanPollTimer = null;

    // 初始化 24 个 Profile 槽位下拉选项
    for (let i = 0; i < 24; i += 1) {
      const option = document.createElement('option');
      option.value = String(i);
      option.textContent = `槽位 ${i}`;
      slotSelect.appendChild(option);
    }

    // 将当前 UART 参数渲染到表单显示区
    function renderCurrent(data) {
      currentEl.textContent =
        `波特率: ${data.baudRate} | 数据位: ${data.dataBits} | 校验位: ${data.parity} | 停止位: ${data.stopBits}`;
      form.baudRate.value = data.baudRate;
      baudRatePresetEl.value = commonBaudRates.includes(String(data.baudRate)) ? String(data.baudRate) : 'custom';
      form.dataBits.value = String(data.dataBits);
      form.parity.value = data.parity;
      form.stopBits.value = String(data.stopBits);
    }

    // 下拉选择变化时同步到手动输入框
    baudRatePresetEl.addEventListener('change', () => {
      if (baudRatePresetEl.value !== 'custom') {
        form.baudRate.value = baudRatePresetEl.value;
      }
    });

    // 手动输入时同步下拉框为"自定义"（若不是常用值）
    form.baudRate.addEventListener('input', () => {
      const baudRate = String(form.baudRate.value);
      baudRatePresetEl.value = commonBaudRates.includes(baudRate) ? baudRate : 'custom';
    });

    // 帮助弹窗控制
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
    // 点击遮罩层也可关闭
    helpModalEl.addEventListener('click', (event) => {
      if (event.target === helpModalEl) {
        closeHelpModal();
      }
    });
    // ESC 键关闭弹窗
    window.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && helpModalEl.classList.contains('open')) {
        closeHelpModal();
      }
    });

    // 从设备获取当前 UART 参数并渲染到表单
    async function refresh() {
      const response = await fetch('/api/uart');
      if (!response.ok) {
        throw new Error('读取当前串口参数失败');
      }
      const data = await response.json();
      renderCurrent(data);
    }

    // HTML 转义表：防止 XSS 注入（SSID/密码等用户内容必须转义）
    const htmlEscapes = {
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;',
    };

    // 对任意字符串进行 HTML 安全转义
    function escapeHtml(value) {
      return String(value).replace(/[&<>"']/g, (char) => htmlEscapes[char]);
    }

    // 生成单个 Profile 卡片的 HTML 字符串（SSID 经转义）
    function profileCard(profile) {
      const active = profile.active ? '当前启用' : '已保存';
      const index = escapeHtml(profile.index);
      const ssid = escapeHtml(profile.ssid || '');
      return `
        <div class="profile">
          <div class="profile-title">槽位 ${index} - ${ssid}</div>
          <div class="profile-meta">${active}</div>
          <div class="profile-actions">
            <button type="button" data-action="activate" data-index="${index}">启用</button>
            <button type="button" data-action="delete" data-index="${index}">删除</button>
          </div>
        </div>`;
    }

    // 渲染 Wi-Fi 状态与 Profile 列表
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
      scanResultsEl.innerHTML = sortedNetworks.map((network) => {
        const rawSsid = network.ssid || '';
        const title = rawSsid || '<hidden>';
        return `
        <div class="scan-item" data-ssid="${escapeHtml(rawSsid)}" data-open="${network.open ? '1' : '0'}">
          <div class="scan-title">${escapeHtml(title)}</div>
          <div class="scan-meta">信号: ${escapeHtml(network.rssi)} dBm | 信道: ${escapeHtml(network.channel)} | 安全: ${escapeHtml(network.security)}</div>
        </div>
      `;
      }).join('');
    }

    async function fetchScanResults() {
      const response = await fetch('/api/wifi/scan');
      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.error || '读取扫描结果失败');
      }
      return data;
    }

    async function pollScanUntilComplete() {
      try {
        const data = await fetchScanResults();
        if (data.scanning) {
          scanStatusEl.textContent = '正在扫描附近 Wi-Fi...';
          scanPollTimer = window.setTimeout(pollScanUntilComplete, 500);
          return;
        }

        scanPollTimer = null;
        renderScanResults(data);
        scanStatusEl.textContent = `共扫描到 ${data.networks.length} 个热点。`;
      } catch (error) {
        scanPollTimer = null;
        scanStatusEl.textContent = error.message;
      }
    }

    function updatePasswordHint(isOpenNetwork) {
      wifiPasswordHintEl.textContent = isOpenNetwork
        ? '已选择开放网络，可以不填写密码。'
        : '密码留空默认保留旧密码；如果要开放网络或清空密码，请选择保存为空密码。';
    }

    async function fetchScanResults() {
      const response = await fetch('/api/wifi/scan');
      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.error || '读取扫描结果失败');
      }
      return data;
    }

    async function pollScanUntilComplete() {
      try {
        const data = await fetchScanResults();
        if (data.scanning) {
          scanStatusEl.textContent = '正在扫描附近 Wi-Fi...';
          scanPollTimer = window.setTimeout(pollScanUntilComplete, 500);
          return;
        }

        scanPollTimer = null;
        renderScanResults(data);
        scanStatusEl.textContent = `共扫描到 ${data.networks.length} 个热点。`;
      } catch (error) {
        scanPollTimer = null;
        scanStatusEl.textContent = error.message;
      }
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
        wifiForm.keepPassword.value = '0';
      }
      wifiForm.activate.value = '1';
      updatePasswordHint(isOpenNetwork);
      wifiStatusEl.textContent = `已选择 ${ssid || '<hidden>'} 并填入 Wi-Fi 表单。`;
    });

    scanButton.addEventListener('click', async () => {
      scanStatusEl.textContent = '正在扫描附近 Wi-Fi...';
      if (scanPollTimer !== null) {
        window.clearTimeout(scanPollTimer);
        scanPollTimer = null;
      }

      try {
        const response = await fetch('/api/wifi/scan', { method: 'POST' });
        const data = await response.json();
        if (!response.ok) {
          throw new Error(data.error || '扫描失败');
        }
        if (data.scanning) {
          scanPollTimer = window.setTimeout(pollScanUntilComplete, 500);
        } else {
          renderScanResults(data);
          scanStatusEl.textContent = `共扫描到 ${data.networks.length} 个热点。`;
        }
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

}
