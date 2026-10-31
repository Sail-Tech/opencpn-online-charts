'use strict';

const GPSCache = (() => {
  const CACHE_NAME = 'gps-charts-resources-v1';
  const INDEX_KEY = 'chartCacheIndex';
  const PROTOCOL = 'cached';
  const PREFIX = `${PROTOCOL}://`;
  const DEFAULT_LIMIT_MB = 256;
  const CACHEABLE_ORIGINS = [
    'https://tile.openstreetmap.org/',
    'https://tiles.openwaters.io/seascape/',
    'https://tiles.openwaters.io/fonts/'
  ];

  let enabled = true;
  let limitBytes = DEFAULT_LIMIT_MB * 1024 * 1024;
  let index = {};
  let totalBytes = 0;
  let initializePromise = null;
  let cachePromise = null;
  let registered = false;

  function isCacheable(url) {
    return CACHEABLE_ORIGINS.some(prefix => url.startsWith(prefix));
  }

  function transformRequest(url) {
    if (enabled && isCacheable(url)) return { url: `${PREFIX}${url}` };
    return { url };
  }

  function configure(config = {}) {
    enabled = config.cacheEnabled !== false;
    const mb = Number(config.cacheMaxMB) || DEFAULT_LIMIT_MB;
    limitBytes = Math.max(16, Math.min(2048, mb)) * 1024 * 1024;
  }

  async function cache() {
    if (!cachePromise) cachePromise = caches.open(CACHE_NAME);
    return cachePromise;
  }

  function recalculateTotal() {
    totalBytes = Object.values(index).reduce((sum, item) => sum + (Number(item.size) || 0), 0);
  }

  async function saveIndex() {
    await chrome.storage.local.set({ [INDEX_KEY]: index });
  }

  async function evict() {
    if (totalBytes <= limitBytes) return;
    const oldest = Object.entries(index).sort((a, b) => a[1].lastUsed - b[1].lastUsed);
    const store = await cache();
    for (const [url, item] of oldest) {
      if (totalBytes <= limitBytes) break;
      await store.delete(url);
      totalBytes -= Number(item.size) || 0;
      delete index[url];
    }
    totalBytes = Math.max(0, totalBytes);
    await saveIndex();
  }

  async function initialize(config) {
    if (config) configure(config);
    if (!initializePromise) {
      initializePromise = (async () => {
        const stored = await chrome.storage.local.get(INDEX_KEY);
        index = stored[INDEX_KEY] && typeof stored[INDEX_KEY] === 'object' ? stored[INDEX_KEY] : {};
        recalculateTotal();
        await evict();
      })().catch(error => {
        initializePromise = null;
        throw error;
      });
    }
    await initializePromise;
    if (config) await evict();
  }

  async function store(url, data, contentType) {
    const size = data.byteLength;
    if (!enabled || size === 0 || size > limitBytes) return;
    try {
      const response = new Response(data.slice(0), {
        status: 200,
        headers: { 'Content-Type': contentType || 'application/octet-stream' }
      });
      const store = await cache();
      await store.put(url, response);
      const previousSize = Number(index[url]?.size) || 0;
      index[url] = { size, lastUsed: Date.now() };
      totalBytes = Math.max(0, totalBytes - previousSize + size);
      await saveIndex();
      await evict();
    } catch (error) {
      console.warn(`Chart cache write skipped: ${error.message}`);
    }
  }

  function responseData(url, data) {
    if (url.endsWith('.json')) return JSON.parse(new TextDecoder().decode(data));
    return data;
  }

  async function handleRequest(params, abortController) {
    await initialize();
    const url = params.url.startsWith(PREFIX) ? params.url.slice(PREFIX.length) : params.url;
    const cached = enabled ? await (await cache()).match(url) : null;

    if (cached && navigator.onLine === false) {
      return { data: responseData(url, await cached.arrayBuffer()) };
    }

    try {
      const response = await fetch(url, {
        signal: abortController?.signal,
        credentials: 'omit',
        cache: 'default'
      });
      if (!response.ok) throw new Error(`HTTP ${response.status} for ${url}`);
      const data = await response.arrayBuffer();
      if (enabled) await store(url, data, response.headers.get('Content-Type'));
      return { data: responseData(url, data) };
    } catch (error) {
      if (cached && error.name !== 'AbortError') {
        return { data: responseData(url, await cached.arrayBuffer()) };
      }
      throw error;
    }
  }

  function register() {
    if (registered) return;
    maplibregl.addProtocol(PROTOCOL, handleRequest);
    registered = true;
  }

  async function clear() {
    await caches.delete(CACHE_NAME);
    cachePromise = null;
    index = {};
    totalBytes = 0;
    await saveIndex();
  }

  async function stats() {
    await initialize();
    return { entries: Object.keys(index).length, bytes: totalBytes, limitBytes, enabled };
  }

  return { initialize, configure, register, transformRequest, clear, stats, isCacheable };
})();
