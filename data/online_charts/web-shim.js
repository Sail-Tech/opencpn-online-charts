'use strict';

// Browser host adapter: retains the extension dashboard's storage and messaging
// interface while using only local browser APIs and the OpenCPN loopback server.
window.OPENCPN_PLUGIN = true;
(() => {
  const STORAGE_KEY = 'online-charts-settings-v1';
  const DEFAULTS = {
    provider: 'iboating',
    zoom: 13.76,
    autoCenterProvider: true,
    updateSeconds: 5,
    wsUrl: `ws://${location.host}/gps`,
    serialBaud: 4800,
    serialPortId: '',
    bathymetryEnabled: true,
    cacheEnabled: true,
    cacheMaxMB: 256
  };
  let providerWindow = null;
  let providerAutoCenter = true;
  let latestFix = null;
  let lastProviderUpdate = 0;
  let lastProviderPosition = null;

  function read() {
    try {
      const value = JSON.parse(localStorage.getItem(STORAGE_KEY) || '{}');
      return value && typeof value === 'object' ? value : {};
    } catch (_) {
      return {};
    }
  }

  async function write(values) {
    const updated = { ...read(), ...values };
    localStorage.setItem(STORAGE_KEY, JSON.stringify(updated));
    return updated;
  }

  function selected(request, all) {
    if (request === undefined || request === null) return all;
    if (typeof request === 'string') return { [request]: all[request] };
    if (Array.isArray(request)) {
      return Object.fromEntries(request.map(key => [key, all[key]]));
    }
    if (typeof request === 'object') {
      return Object.fromEntries(Object.entries(request).map(([key, fallback]) => [key, all[key] === undefined ? fallback : all[key]]));
    }
    return {};
  }

  function providerUrl(provider, fix, zoom) {
    if (provider === 'cmap') {
      return `https://appchart.c-map.com/core/select/new_place/@${Number(fix.lon).toFixed(6)},${Number(fix.lat).toFixed(6)}`;
    }
    return `https://fishing-app.gpsnauticalcharts.com/i-boating-fishing-web-app/fishing-marine-charts-navigation.html#${Number(zoom) || 13.76}/${Number(fix.lat).toFixed(6)}/${Number(fix.lon).toFixed(6)}`;
  }

  function distanceMeters(a, b) {
    if (!a || !b) return Infinity;
    const rad = Math.PI / 180;
    const dLat = (b.lat - a.lat) * rad;
    const dLon = (b.lon - a.lon) * rad;
    const lat1 = a.lat * rad;
    const lat2 = b.lat * rad;
    const h = Math.sin(dLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2;
    return 12742000 * Math.asin(Math.min(1, Math.sqrt(h)));
  }

  function updateProvider(config, force = false) {
    if (!providerWindow || providerWindow.closed || !latestFix) return;
    const now = Date.now();
    const moved = distanceMeters(lastProviderPosition, latestFix);
    if (!force && (!providerAutoCenter || now - lastProviderUpdate < Number(config.updateSeconds || 5) * 1000)) return;
    if (!force && moved < 2 && lastProviderPosition?.zoom === config.zoom) return;
    try {
      providerWindow.location.href = providerUrl(config.provider, latestFix, config.zoom);
      lastProviderUpdate = now;
      lastProviderPosition = { lat: latestFix.lat, lon: latestFix.lon, zoom: config.zoom };
    } catch (_) {
      providerWindow = null;
    }
  }

  async function message(request) {
    if (!request || typeof request !== 'object') return { ok: false, error: 'Unknown message' };
    if (request.type === 'GPS_FIX') {
      latestFix = request.fix;
      await write({ latestFix });
      updateProvider({ ...DEFAULTS, ...read() });
      return { ok: true };
    }
    if (request.type === 'SETTINGS_CHANGED') {
      updateProvider({ ...DEFAULTS, ...read() }, true);
      return { ok: true };
    }
    if (request.type === 'OPEN_PROVIDER') {
      const config = { ...DEFAULTS, ...read() };
      latestFix = latestFix || config.latestFix || { lat: 50.775, lon: -1.3 };
      providerAutoCenter = request.follow ?? config.autoCenterProvider;
      await write({ autoCenterProvider: providerAutoCenter });
      const url = providerUrl(request.provider || config.provider, latestFix, config.zoom);
      if (!providerWindow || providerWindow.closed) providerWindow = window.open(url, '_blank');
      else {
        providerWindow.location.href = url;
        providerWindow.focus();
      }
      if (!providerWindow) return { ok: false, error: 'The browser blocked the provider window. Allow pop-ups for this local page and try again.' };
      lastProviderUpdate = Date.now();
      lastProviderPosition = { lat: latestFix.lat, lon: latestFix.lon, zoom: config.zoom };
      return { ok: true };
    }
    return { ok: true };
  }

  window.chrome = {
    storage: {
      local: {
        async get(request) {
          return selected(request, { ...DEFAULTS, ...read() });
        },
        async set(values) {
          await write(values);
        }
      }
    },
    runtime: {
      getURL(path) {
        return new URL(path, location.href).href;
      },
      sendMessage: message
    }
  };
})();
