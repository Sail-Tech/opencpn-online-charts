'use strict';

const $ = id => document.getElementById(id);
const DEFAULT_FIX = { lat: 50.775, lon: -1.3 };
const SETTING_IDS = [
  'provider', 'zoom', 'autoCenterProvider', 'updateSeconds', 'wsUrl',
  'serialBaud', 'serialPortId', 'bathymetryEnabled', 'cacheEnabled', 'cacheMaxMB'
];
const OSM_MAX_ZOOM = 19;
let config = {};
let map = null;
let mapReady = false;
let boatMarker = null;
let track = [];
let lastFix = null;
let socket = null;
let serialPort = null;
let serialReader = null;
let serialReadTask = null;
let serialStopped = false;
let intentionallyDisconnected = false;
let reconnectTimer = null;
let availablePorts = [];

const parser = new GPSParser.Parser(receiveFix, (message, raw) => {
  log(`${message}: ${String(raw).slice(0, 90)}`);
});

function log(message) {
  $('eventLog').textContent = `${new Date().toLocaleTimeString('en-GB')} · ${message}`;
}

function setConnectionState(state, text) {
  $('statusDot').className = `dot ${state === 'live' ? 'live' : state === 'error' ? 'error' : ''}`;
  $('statusText').textContent = text;
  $('connect').disabled = state === 'live' || state === 'connecting';
  $('disconnect').disabled = state !== 'live' && state !== 'connecting' && state !== 'error';
}

function formatNumber(value, digits, suffix = '') {
  if (value === null || value === undefined || value === '' || !Number.isFinite(Number(value))) return '—';
  return `${Number(value).toFixed(digits)}${suffix}`;
}

function formatBytes(bytes) {
  const value = Number(bytes) || 0;
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`;
  if (value < 1024 * 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(1)} MB`;
  return `${(value / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

function updateClock() {
  if (!lastFix) {
    $('clock').textContent = '';
    return;
  }
  const age = Math.max(0, Math.round((Date.now() - new Date(lastFix.receivedAt).getTime()) / 1000));
  $('clock').textContent = age < 5 ? 'GPS updated now' : `Last position ${age} s ago`;
}

function receiveFix(fix) {
  lastFix = fix;
  $('lat').textContent = formatNumber(fix.lat, 6, '°');
  $('lon').textContent = formatNumber(fix.lon, 6, '°');
  $('sog').textContent = formatNumber(fix.sog, 1, ' kn');
  $('cog').textContent = formatNumber(fix.cog ?? fix.heading, 0, '°');
  $('fixMeta').textContent = `${fix.quality || 'GPS'} · ${fix.source} · ${new Date(fix.receivedAt).toLocaleTimeString('en-GB')}`;
  $('mapNotice').style.opacity = '0';
  setConnectionState('live', `GPS active · ${fix.source}`);
  updateMap(fix);
  updateClock();
  chrome.runtime.sendMessage({ type: 'GPS_FIX', fix }).catch(error => log(`State not synchronized: ${error.message}`));
}

function boatElement() {
  const element = document.createElement('div');
  element.className = 'boat-marker';
  element.innerHTML = '<svg width="34" height="34" viewBox="0 0 34 34" aria-label="Vessel"><circle cx="17" cy="17" r="15" fill="#fff" stroke="#073b55" stroke-width="2"/><path d="M17 4 L25 27 L17 22 L9 27 Z" fill="#ef4d4d" stroke="#9e2727" stroke-width="1"/><circle cx="17" cy="17" r="2.2" fill="#073b55"/></svg>';
  return element;
}

function initializeMap() {
  maplibregl.setWorkerUrl(chrome.runtime.getURL('vendor/maplibre-gl-csp-worker.js'));
  GPSCache.register();
  if (map) {
    map.remove();
    map = null;
    boatMarker = null;
    mapReady = false;
  }
  const center = lastFix || DEFAULT_FIX;
  map = new maplibregl.Map({
    container: 'map',
    transformRequest: url => GPSCache.transformRequest(url),
    style: {
      version: 8,
      glyphs: 'https://tiles.openwaters.io/fonts/{fontstack}/{range}.pbf',
      sources: {
        ...GPSBathymetry.sources(),
        chart: {
          type: 'raster',
          tiles: ['https://tile.openstreetmap.org/{z}/{x}/{y}.png'],
          tileSize: 256,
          minzoom: 0,
          maxzoom: OSM_MAX_ZOOM,
          attribution: '© OpenStreetMap'
        }
      },
      layers: [
        { id: 'background', type: 'background', paint: { 'background-color': '#d8e5ea' } },
        { id: 'chart', type: 'raster', source: 'chart' },
        ...GPSBathymetry.layers(config)
      ]
    },
    center: [center.lon, center.lat],
    zoom: Math.min(Number(config.zoom) || 13.76, OSM_MAX_ZOOM),
    attributionControl: false
  });
  map.addControl(new maplibregl.NavigationControl({ visualizePitch: false }), 'top-right');
  map.addControl(new maplibregl.AttributionControl({ compact: true }), 'bottom-right');
  map.on('style.load', () => {
    GPSBathymetry.applyVisibility(map, config);
    map.addSource('track', {
      type: 'geojson',
      data: { type: 'Feature', geometry: { type: 'LineString', coordinates: track } }
    });
    map.addLayer({
      id: 'track', type: 'line', source: 'track',
      layout: { 'line-cap': 'round', 'line-join': 'round' },
      paint: { 'line-color': '#e63946', 'line-width': 3, 'line-opacity': 0.85 }
    });
    mapReady = true;
    if (lastFix) updateMap(lastFix);
  });
  map.on('error', event => {
    const message = event?.error?.message || 'Chart resource error';
    log(message.slice(0, 150));
  });
  map.on('dragstart', () => { $('followBoat').checked = false; });
}

function updateMap(fix) {
  if (!map || !mapReady) return;
  const position = [fix.lon, fix.lat];
  if (!boatMarker) {
    boatMarker = new maplibregl.Marker({ element: boatElement(), rotationAlignment: 'map' })
      .setLngLat(position).addTo(map);
  } else {
    boatMarker.setLngLat(position);
  }
  boatMarker.setRotation(Number(fix.cog ?? fix.heading) || 0);
  const previous = track[track.length - 1];
  if (!previous || Math.abs(previous[0] - fix.lon) > 0.000005 || Math.abs(previous[1] - fix.lat) > 0.000005) {
    track.push(position);
    if (track.length > 1000) track = track.slice(-1000);
    map.getSource('track').setData({
      type: 'Feature', geometry: { type: 'LineString', coordinates: track }
    });
  }
  if ($('followBoat').checked) {
    map.easeTo({ center: position, duration: 650, zoom: Math.min(Number(config.zoom) || 13.76, OSM_MAX_ZOOM) });
  }
}

function updateModeRows() {
  const serial = $('connectionMode').value === 'serial';
  $('wsRow').classList.toggle('hidden', serial);
  $('serialPortRow').classList.toggle('hidden', !serial);
  $('baudRow').classList.toggle('hidden', !serial);
  if (serial) scanSerialPorts({ silent: true }).catch(error => log(error.message));
}

async function saveSetting(id) {
  const element = $(id);
  if (!element) return;
  let value;
  if (element.type === 'checkbox') value = element.checked;
  else if (element.type === 'number') value = Number(element.value);
  else value = element.value;
  config[id] = value;
  await chrome.storage.local.set({ [id]: value });
  if (id === 'bathymetryEnabled') {
    GPSBathymetry.applyVisibility(map, config);
    log(value ? 'Online vector bathymetry enabled.' : 'Vector bathymetry disabled.');
  }
  if (id === 'cacheEnabled' || id === 'cacheMaxMB') {
    await GPSCache.initialize(config);
    await updateCacheStatus();
    log(id === 'cacheEnabled'
      ? (value ? 'Offline chart cache enabled.' : 'Offline chart cache disabled.')
      : `Cache limit set to ${value} MB.`);
  }
  if (['provider', 'zoom', 'autoCenterProvider', 'updateSeconds'].includes(id)) {
    await chrome.runtime.sendMessage({ type: 'SETTINGS_CHANGED' });
  }
}

async function updateCacheStatus() {
  const stats = await GPSCache.stats();
  $('cacheStatus').textContent = stats.enabled
    ? `${formatBytes(stats.bytes)} · ${stats.entries} resources · Limit ${formatBytes(stats.limitBytes)}`
    : `Cache disabled · ${formatBytes(stats.bytes)} retained until cleared`;
}

function serialPortKey(port, index) {
  const info = port.getInfo();
  if (info.usbVendorId !== undefined) {
    const vendor = info.usbVendorId.toString(16).padStart(4, '0');
    const product = (info.usbProductId || 0).toString(16).padStart(4, '0');
    return `usb:${vendor}:${product}:${index}`;
  }
  return `serial:${index}`;
}

function serialPortLabel(port, index) {
  const info = port.getInfo();
  if (info.usbVendorId !== undefined) {
    const vendor = info.usbVendorId.toString(16).padStart(4, '0').toUpperCase();
    const product = (info.usbProductId || 0).toString(16).padStart(4, '0').toUpperCase();
    return `Port ${index + 1} · USB ${vendor}:${product}`;
  }
  return `Serial port ${index + 1}`;
}

async function scanSerialPorts({ silent = false } = {}) {
  if (!('serial' in navigator)) {
    availablePorts = [];
    $('serialPortSelect').innerHTML = '<option value="">Web Serial is unavailable</option>';
    if (!silent) log('Web Serial is not available in this browser.');
    return [];
  }
  availablePorts = await navigator.serial.getPorts();
  const select = $('serialPortSelect');
  select.innerHTML = '';
  if (!availablePorts.length) {
    select.innerHTML = '<option value="">No authorized port found</option>';
  } else {
    availablePorts.forEach((port, index) => {
      const option = document.createElement('option');
      option.value = serialPortKey(port, index);
      option.textContent = serialPortLabel(port, index);
      select.appendChild(option);
    });
    const preferred = availablePorts.some((port, index) => serialPortKey(port, index) === config.serialPortId);
    select.value = preferred ? config.serialPortId : serialPortKey(availablePorts[0], 0);
    if (!preferred && select.value) {
      config.serialPortId = select.value;
      await chrome.storage.local.set({ serialPortId: select.value });
    }
  }
  if (!silent) {
    log(availablePorts.length
      ? `${availablePorts.length} authorized serial port(s) found.`
      : 'No authorized ports found. Use Add / authorize to grant access.');
  }
  return availablePorts;
}

async function authorizeSerialPort() {
  if (!('serial' in navigator)) throw new Error('Web Serial is not available in this browser.');
  log('Select a serial device in Chrome’s device chooser.');
  const port = await navigator.serial.requestPort();
  await scanSerialPorts({ silent: true });
  const index = availablePorts.indexOf(port);
  if (index >= 0) {
    const id = serialPortKey(port, index);
    $('serialPortSelect').value = id;
    config.serialPortId = id;
    await chrome.storage.local.set({ serialPortId: id });
  }
  log('Serial port authorized and selected.');
}

function scheduleReconnect() {
  if (!window.OPENCPN_PLUGIN || intentionallyDisconnected || reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectWebSocket().catch(error => log(error.message));
  }, 3000);
}

async function connectWebSocket() {
  intentionallyDisconnected = false;
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  const url = config.wsUrl.trim();
  if (!/^wss?:\/\//i.test(url)) throw new Error('The address must start with ws:// or wss://');
  setConnectionState('connecting', 'Connecting WebSocket…');
  socket = new WebSocket(url);
  socket.onopen = () => {
    setConnectionState('live', 'WebSocket connected · waiting for GPS');
    log(`Connected to ${url}`);
  };
  socket.onmessage = async event => {
    const text = typeof event.data === 'string' ? event.data : await event.data.text();
    parser.push(`${text}\n`, 'WebSocket');
  };
  socket.onerror = () => {
    setConnectionState('error', 'WebSocket connection error');
    log('Could not communicate with the WebSocket.');
  };
  socket.onclose = () => {
    socket = null;
    if (!intentionallyDisconnected) {
      setConnectionState('error', 'WebSocket disconnected · retrying');
      log('The WebSocket connection ended. Reconnecting in 3 seconds…');
      scheduleReconnect();
    }
  };
}

async function connectSerial() {
  if (!('serial' in navigator)) throw new Error('Web Serial is not available in this Chrome browser.');
  intentionallyDisconnected = false;
  await scanSerialPorts({ silent: true });
  let selected = availablePorts.find((port, index) => serialPortKey(port, index) === $('serialPortSelect').value);
  if (!selected) {
    setConnectionState('connecting', 'Select a serial port…');
    selected = await navigator.serial.requestPort();
    await scanSerialPorts({ silent: true });
    const index = availablePorts.indexOf(selected);
    if (index >= 0) {
      config.serialPortId = serialPortKey(selected, index);
      $('serialPortSelect').value = config.serialPortId;
      await chrome.storage.local.set({ serialPortId: config.serialPortId });
    }
  }
  serialStopped = false;
  serialPort = selected;
  await serialPort.open({ baudRate: Number(config.serialBaud) || 4800 });
  setConnectionState('live', `Serial connected · ${config.serialBaud} baud`);
  log(`Receiving data from ${serialPortLabel(serialPort, Math.max(0, availablePorts.indexOf(serialPort)))}.`);

  serialReadTask = (async () => {
    const decoder = new TextDecoderStream();
    const closed = serialPort.readable.pipeTo(decoder.writable);
    serialReader = decoder.readable.getReader();
    try {
      while (!serialStopped) {
        const { value, done } = await serialReader.read();
        if (done) break;
        if (value) parser.push(value, 'Serial port');
      }
    } catch (error) {
      if (!serialStopped) {
        log(`Serial error: ${error.message}`);
        setConnectionState('error', 'Serial port error');
      }
    } finally {
      try { serialReader.releaseLock(); } catch (_) {}
      serialReader = null;
      await closed.catch(() => {});
    }
  })();
}

async function disconnect() {
  intentionallyDisconnected = true;
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  serialStopped = true;
  if (socket) {
    const current = socket;
    socket = null;
    current.close();
  }
  if (serialReader) {
    try { await serialReader.cancel(); } catch (_) {}
  }
  if (serialReadTask) {
    try { await serialReadTask; } catch (_) {}
    serialReadTask = null;
  }
  if (serialPort) {
    try { await serialPort.close(); } catch (_) {}
    serialPort = null;
  }
  setConnectionState('idle', 'No GPS connection');
  log('Connection closed by the user.');
}

async function connect() {
  $('connect').disabled = true;
  try {
    if ($('connectionMode').value === 'serial') await connectSerial();
    else await connectWebSocket();
  } catch (error) {
    setConnectionState('error', 'Connection failed');
    log(error.message);
    if (serialPort) {
      try { await serialPort.close(); } catch (_) {}
      serialPort = null;
    }
  }
}

async function initialize() {
  config = await chrome.storage.local.get([...SETTING_IDS, 'latestFix']);
  if (config.cacheEnabled === undefined) config.cacheEnabled = true;
  if (config.cacheMaxMB === undefined) config.cacheMaxMB = 256;
  for (const id of SETTING_IDS) {
    const element = $(id);
    if (!element || config[id] === undefined) continue;
    if (element.type === 'checkbox') element.checked = Boolean(config[id]);
    else element.value = config[id];
    element.addEventListener('change', () => saveSetting(id).catch(error => log(error.message)));
  }
  $('connectionMode').addEventListener('change', updateModeRows);
  $('serialPortSelect').addEventListener('change', async () => {
    config.serialPortId = $('serialPortSelect').value;
    await chrome.storage.local.set({ serialPortId: config.serialPortId });
    log('Preferred serial port saved.');
  });
  $('scanSerial').addEventListener('click', () => scanSerialPorts().catch(error => log(error.message)));
  $('authorizeSerial').addEventListener('click', () => authorizeSerialPort().catch(error => {
    log(error.name === 'NotFoundError' ? 'No serial port was selected.' : error.message);
  }));
  updateModeRows();
  $('connect').addEventListener('click', connect);
  $('disconnect').addEventListener('click', disconnect);
  $('simulate').addEventListener('click', () => {
    const fix = { lat: 50.775, lon: -1.3, sog: 5.2, cog: 45, source: 'Simulation', quality: 'TEST', receivedAt: new Date().toISOString() };
    receiveFix(fix);
    log('Test position applied in the Solent, off Cowes.');
  });
  $('openProvider').addEventListener('click', async () => {
    await Promise.all(['provider', 'zoom', 'autoCenterProvider', 'updateSeconds'].map(saveSetting));
    const response = await chrome.runtime.sendMessage({ type: 'OPEN_PROVIDER', provider: config.provider, follow: config.autoCenterProvider });
    if (!response?.ok) log(response?.error || 'Could not open the provider.');
    else log(`${config.provider === 'cmap' ? 'C-MAP Pin Location' : 'i-Boating'} opened.`);
  });
  $('centerBoat').addEventListener('click', () => {
    if (!lastFix) {
      log('No GPS position is available for centering.');
      return;
    }
    if (!mapReady) {
      log('The chart is not ready yet. Try again shortly.');
      return;
    }
    $('followBoat').checked = true;
    updateMap(lastFix);
    log('Chart centered on the vessel; automatic following enabled.');
  });
  $('clearTrack').addEventListener('click', () => {
    track = [];
    if (mapReady) map.getSource('track').setData({ type: 'Feature', geometry: { type: 'LineString', coordinates: [] } });
    log('Track cleared.');
  });
  $('clearCache').addEventListener('click', async () => {
    if (!window.confirm('Delete all cached OSM and Open Waters chart resources?')) return;
    await GPSCache.clear();
    await updateCacheStatus();
    log('Offline chart cache cleared.');
  });
  if ('serial' in navigator) {
    navigator.serial.addEventListener('connect', () => scanSerialPorts({ silent: true }).catch(error => log(error.message)));
    navigator.serial.addEventListener('disconnect', () => scanSerialPorts({ silent: true }).catch(error => log(error.message)));
  }
  window.addEventListener('online', () => log('Internet connection restored; chart resources will be refreshed.'));
  window.addEventListener('offline', () => log('Internet connection lost; previously cached chart resources will be used.'));
  if (config.latestFix && Number.isFinite(config.latestFix.lat)) {
    lastFix = config.latestFix;
    receiveFix(lastFix);
    setConnectionState('idle', 'Last saved position · GPS stopped');
  }
  await GPSCache.initialize(config);
  await updateCacheStatus();
  initializeMap();
  if (window.OPENCPN_PLUGIN) {
    log('Connecting to the OpenCPN position stream…');
    await connectWebSocket();
  }
  setInterval(updateClock, 1000);
  setInterval(() => updateCacheStatus().catch(() => {}), 5000);
}

window.addEventListener('beforeunload', () => {
  intentionallyDisconnected = true;
  serialStopped = true;
  if (socket) socket.close();
  if (serialReader) serialReader.cancel().catch(() => {});
});

initialize().catch(error => {
  log(`Initialization failed: ${error.message}`);
  setConnectionState('error', 'Initialization error');
});
