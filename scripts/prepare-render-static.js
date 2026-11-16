/**
 * Prepares separated static site directories for Render Static Site deployment:
 *   - dist/customer     (Landing Gateway + Customer Portal + shared assets)
 *   - dist/manufacturer (Manufacturer & Superadmin Portal + shared assets)
 *
 * Also injects a lightweight runtime API/WebSocket bridge (`render-api-bridge.js`)
 * when `API_BASE_URL` (e.g., https://boda360-backend-api.onrender.com) is provided,
 * so relative `/api/*` fetch calls and `/ws/telemetry` WebSocket connections
 * automatically target the Render Backend Web Service.
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const DIST = path.join(ROOT, 'dist');
const API_BASE_URL = (process.env.API_BASE_URL || '').replace(/\/+$/, '');

function copyRecursive(src, dest) {
  if (!fs.existsSync(src)) return;
  const stat = fs.statSync(src);
  if (stat.isDirectory()) {
    fs.mkdirSync(dest, { recursive: true });
    for (const entry of fs.readdirSync(src)) {
      copyRecursive(path.join(src, entry), path.join(dest, entry));
    }
  } else {
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.copyFileSync(src, dest);
  }
}

function buildApiBridgeScript(apiBaseUrl) {
  return `/* Auto-generated Render Cross-Service API & WebSocket Bridge */
(function () {
  var isLocalHost = window.location.hostname === 'localhost' || window.location.hostname === '127.0.0.1';
  var cloudBase = (${JSON.stringify(apiBaseUrl)} || window.localStorage.getItem('BODA_API_BASE_URL') || 'https://boda360-backend-api.onrender.com').replace(/\\/+$/, '');
  var localBase = 'http://localhost:3000';
  var activeBase = isLocalHost ? '' : cloudBase;
  window.BODA_API_BASE_URL = activeBase;

  var origFetch = window.fetch ? window.fetch.bind(window) : null;

  // Probe local USB-Serial gateway (http://localhost:3000) so if the user is viewing the Render URL
  // on the same PC where COM7 is plugged in, COM7 & Cloud MQTT both work seamlessly!
  if (!isLocalHost && origFetch) {
    origFetch(localBase + '/healthz', { mode: 'cors' })
      .then(function (r) {
        if (r && r.ok) {
          activeBase = localBase;
          window.BODA_API_BASE_URL = localBase;
        }
      })
      .catch(function () {});
  }

  if (!isLocalHost && origFetch) {
    window.fetch = function (input, init) {
      if (typeof input === 'string' && input.indexOf('/api/') === 0) {
        var apiPath = input;
        init = Object.assign({}, init || {});

        // For Engine Cut / Restore commands, fire to BOTH local COM7 gateway and Cloud MQTT gateway
        if (apiPath.indexOf('/api/command/') === 0 && activeBase !== cloudBase) {
          origFetch(cloudBase + apiPath, Object.assign({}, init)).catch(function () {});
        } else if (apiPath.indexOf('/api/command/') === 0 && activeBase === cloudBase) {
          origFetch(localBase + apiPath, Object.assign({}, init)).catch(function () {});
        }

        var primaryUrl = (activeBase || cloudBase) + apiPath;
        var fallbackUrl = (activeBase === localBase ? cloudBase : localBase) + apiPath;

        var ctrl = typeof AbortController !== 'undefined' ? new AbortController() : null;
        var timer = ctrl ? setTimeout(function () { ctrl.abort(); }, 3500) : null;
        var primaryInit = ctrl && !init.signal ? Object.assign({}, init, { signal: ctrl.signal }) : init;

        return origFetch(primaryUrl, primaryInit)
          .then(function (res) {
            if (timer) clearTimeout(timer);
            if (!res.ok && res.status >= 500) {
              return origFetch(fallbackUrl, init);
            }
            return res;
          })
          .catch(function () {
            if (timer) clearTimeout(timer);
            return origFetch(fallbackUrl, init);
          });
      }
      return origFetch(input, init);
    };
  }

  var OrigWebSocket = window.WebSocket;
  if (!isLocalHost && OrigWebSocket) {
    function PatchedWebSocket(url, protocols) {
      if (typeof url === 'string' && url.indexOf('/ws/telemetry') !== -1 && url.indexOf(window.location.host) !== -1) {
        var base = activeBase || cloudBase;
        var wsBase = base.replace(/^http:/i, 'ws:').replace(/^https:/i, 'wss:');
        url = wsBase + '/ws/telemetry';
      }
      return protocols !== undefined ? new OrigWebSocket(url, protocols) : new OrigWebSocket(url);
    }
    PatchedWebSocket.prototype = OrigWebSocket.prototype;
    PatchedWebSocket.CONNECTING = OrigWebSocket.CONNECTING;
    PatchedWebSocket.OPEN = OrigWebSocket.OPEN;
    PatchedWebSocket.CLOSING = OrigWebSocket.CLOSING;
    PatchedWebSocket.CLOSED = OrigWebSocket.CLOSED;
    window.WebSocket = PatchedWebSocket;
  }
})();
`;
}

function injectBridgeIntoHtml(htmlFilePath, scriptRelPath) {
  if (!fs.existsSync(htmlFilePath)) return;
  let html = fs.readFileSync(htmlFilePath, 'utf8');
  const tag = `<script src="${scriptRelPath}"></script>`;
  if (html.includes(tag)) return;
  html = html.replace('<head>', `<head>\n  ${tag}`);
  fs.writeFileSync(htmlFilePath, html, 'utf8');
}

function buildCustomerStaticSite() {
  const outDir = path.join(DIST, 'customer');
  fs.rmSync(outDir, { recursive: true, force: true });
  fs.mkdirSync(outDir, { recursive: true });

  // 1. Copy public landing gateway to root of dist/customer
  copyRecursive(path.join(ROOT, 'public'), outDir);
  // 2. Copy customer-portal to dist/customer/customer
  copyRecursive(path.join(ROOT, 'customer-portal'), path.join(outDir, 'customer'));
  // 3. Copy shared image assets
  copyRecursive(path.join(ROOT, 'image'), path.join(outDir, 'image'));

  // 4. Write & inject API/WebSocket bridge script
  const bridgeScript = buildApiBridgeScript(API_BASE_URL);
  fs.writeFileSync(path.join(outDir, 'render-api-bridge.js'), bridgeScript, 'utf8');
  injectBridgeIntoHtml(path.join(outDir, 'index.html'), '/render-api-bridge.js');
  injectBridgeIntoHtml(path.join(outDir, 'customer', 'index.html'), '/render-api-bridge.js');
  console.log('[Render Build] Built Customer Portal static site -> dist/customer');
}

function buildManufacturerStaticSite() {
  const outDir = path.join(DIST, 'manufacturer');
  fs.rmSync(outDir, { recursive: true, force: true });
  fs.mkdirSync(outDir, { recursive: true });

  // 1. Copy manufacturer-portal to root of dist/manufacturer AND /manufacturer subpath
  copyRecursive(path.join(ROOT, 'manufacturer-portal'), outDir);
  copyRecursive(path.join(ROOT, 'manufacturer-portal'), path.join(outDir, 'manufacturer'));
  // 2. Copy shared public assets (audio, favicon, etc.) & images
  copyRecursive(path.join(ROOT, 'public', 'js'), path.join(outDir, 'js-public'));
  copyRecursive(path.join(ROOT, 'image'), path.join(outDir, 'image'));
  if (fs.existsSync(path.join(ROOT, 'public', 'favicon.svg'))) {
    fs.copyFileSync(path.join(ROOT, 'public', 'favicon.svg'), path.join(outDir, 'favicon.svg'));
  }

  // 3. Write & inject API/WebSocket bridge script
  const bridgeScript = buildApiBridgeScript(API_BASE_URL);
  fs.writeFileSync(path.join(outDir, 'render-api-bridge.js'), bridgeScript, 'utf8');
  injectBridgeIntoHtml(path.join(outDir, 'index.html'), '/render-api-bridge.js');
  injectBridgeIntoHtml(path.join(outDir, 'manufacturer', 'index.html'), '/render-api-bridge.js');
  console.log('[Render Build] Built Manufacturer Portal static site -> dist/manufacturer');
}

buildCustomerStaticSite();
buildManufacturerStaticSite();
