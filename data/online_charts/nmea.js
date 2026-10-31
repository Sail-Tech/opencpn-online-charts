/* NMEA 0183 and simple JSON/Signal K input. No external dependencies. */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.GPSParser = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const KNOTS_PER_MS = 1.9438444924406046;
  const DEGREES_PER_RADIAN = 180 / Math.PI;

  function number(value) {
    if (value === null || value === undefined || value === '') return null;
    const n = Number(value);
    return Number.isFinite(n) ? n : null;
  }

  function validCoordinate(lat, lon) {
    return Number.isFinite(lat) && Number.isFinite(lon) &&
      lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
  }

  function nmeaCoordinate(value, hemisphere, isLatitude) {
    const text = String(value || '');
    if (!/^\d+(?:\.\d+)?$/.test(text)) return null;
    const degreeDigits = isLatitude ? 2 : 3;
    if (text.length < degreeDigits + 2) return null;
    const degrees = Number(text.slice(0, degreeDigits));
    const minutes = Number(text.slice(degreeDigits));
    if (!Number.isFinite(degrees) || !Number.isFinite(minutes) || minutes >= 60) return null;
    let coordinate = degrees + minutes / 60;
    if (hemisphere === 'S' || hemisphere === 'W') coordinate = -coordinate;
    return coordinate;
  }

  function checksumIsValid(sentence) {
    const start = sentence.indexOf('$') >= 0 ? sentence.indexOf('$') : sentence.indexOf('!');
    const star = sentence.indexOf('*');
    if (start < 0 || star < 0) return true;
    const expected = sentence.slice(star + 1, star + 3);
    if (!/^[0-9A-Fa-f]{2}$/.test(expected)) return false;
    let checksum = 0;
    for (let i = start + 1; i < star; i++) checksum ^= sentence.charCodeAt(i);
    return checksum.toString(16).padStart(2, '0').toUpperCase() === expected.toUpperCase();
  }

  function isoFromNmea(time, date) {
    if (!/^\d{6}(?:\.\d+)?$/.test(time || '')) return null;
    const hours = Number(time.slice(0, 2));
    const minutes = Number(time.slice(2, 4));
    const seconds = Number(time.slice(4));
    let year, month, day;
    if (/^\d{6}$/.test(date || '')) {
      day = Number(date.slice(0, 2));
      month = Number(date.slice(2, 4)) - 1;
      const shortYear = Number(date.slice(4, 6));
      year = shortYear >= 80 ? 1900 + shortYear : 2000 + shortYear;
    } else {
      const now = new Date();
      year = now.getUTCFullYear(); month = now.getUTCMonth(); day = now.getUTCDate();
    }
    const result = new Date(Date.UTC(year, month, day, hours, minutes, seconds));
    return Number.isNaN(result.getTime()) ? null : result.toISOString();
  }

  class Parser {
    constructor(onFix, onError) {
      this.onFix = typeof onFix === 'function' ? onFix : function () {};
      this.onError = typeof onError === 'function' ? onError : function () {};
      this.buffer = '';
      this.lastFix = null;
      this.motion = {};
    }

    push(chunk, source) {
      this.buffer += String(chunk || '');
      if (this.buffer.length > 1024 * 1024) this.buffer = this.buffer.slice(-65536);
      const lines = this.buffer.split(/\r\n|\n|\r/);
      this.buffer = lines.pop() || '';
      for (const line of lines) this.processLine(line, source);
    }

    processLine(line, source) {
      const text = String(line || '').trim();
      if (!text) return null;
      try {
        if (text.startsWith('{')) return this.processJson(JSON.parse(text), source, text);
        if (text.startsWith('$') || text.startsWith('!')) return this.processNmea(text, source);
      } catch (error) {
        this.onError(error.message, text);
      }
      return null;
    }

    processNmea(sentence, source) {
      if (!checksumIsValid(sentence)) {
        this.onError('Invalid NMEA checksum', sentence);
        return null;
      }
      const body = sentence.slice(1, sentence.indexOf('*') >= 0 ? sentence.indexOf('*') : undefined);
      const fields = body.split(',');
      const type = fields[0].slice(-3).toUpperCase();
      let fix = null;

      if (type === 'RMC') {
        if (fields[2] !== 'A') return null;
        const lat = nmeaCoordinate(fields[3], fields[4], true);
        const lon = nmeaCoordinate(fields[5], fields[6], false);
        fix = {
          lat, lon,
          sog: number(fields[7]),
          cog: number(fields[8]),
          time: isoFromNmea(fields[1], fields[9]),
          quality: 'RMC'
        };
      } else if (type === 'GGA') {
        if (number(fields[6]) === 0) return null;
        const lat = nmeaCoordinate(fields[2], fields[3], true);
        const lon = nmeaCoordinate(fields[4], fields[5], false);
        fix = {
          lat, lon,
          altitude: number(fields[9]),
          satellites: number(fields[7]),
          hdop: number(fields[8]),
          time: isoFromNmea(fields[1]),
          quality: 'GGA'
        };
      } else if (type === 'GLL') {
        if (fields[6] && fields[6] !== 'A') return null;
        const lat = nmeaCoordinate(fields[1], fields[2], true);
        const lon = nmeaCoordinate(fields[3], fields[4], false);
        fix = { lat, lon, time: isoFromNmea(fields[5]), quality: 'GLL' };
      } else if (type === 'VTG') {
        this.motion = {
          ...this.motion,
          cog: number(fields[1]) ?? this.motion.cog,
          sog: number(fields[5]) ?? this.motion.sog
        };
        if (this.lastFix) fix = { lat: this.lastFix.lat, lon: this.lastFix.lon };
      } else if (type === 'HDT' || type === 'HDG') {
        this.motion = { ...this.motion, cog: number(fields[1]) ?? this.motion.cog };
        if (this.lastFix) fix = { lat: this.lastFix.lat, lon: this.lastFix.lon };
      }

      return fix ? this.emit(fix, source, sentence) : null;
    }

    processJson(data, source, raw) {
      if (!data || typeof data !== 'object') return null;
      if (Array.isArray(data.updates)) return this.processSignalK(data, source, raw);
      const position = data.position && typeof data.position === 'object' ? data.position : data;
      const lat = number(position.lat ?? position.latitude);
      const lon = number(position.lon ?? position.lng ?? position.longitude);
      if (!validCoordinate(lat, lon)) return null;
      return this.emit({
        lat, lon,
        sog: number(data.sog ?? data.speed ?? data.speedOverGround),
        cog: number(data.cog ?? data.course ?? data.courseOverGround),
        heading: number(data.heading),
        altitude: number(data.altitude),
        time: data.time || data.timestamp || null,
        satellites: number(data.satellites ?? data.nSats),
        source: data.source || null,
        quality: data.quality || 'JSON'
      }, source, raw);
    }

    processSignalK(data, source, raw) {
      const merged = {};
      for (const update of data.updates) {
        for (const item of (update.values || [])) {
          const value = item.value;
          if (item.path === 'navigation.position' && value) {
            merged.lat = number(value.latitude);
            merged.lon = number(value.longitude);
          } else if (item.path === 'navigation.speedOverGround' && number(value) !== null) {
            merged.sog = number(value) * KNOTS_PER_MS;
          } else if (item.path === 'navigation.courseOverGroundTrue' && number(value) !== null) {
            merged.cog = number(value) * DEGREES_PER_RADIAN;
          } else if (item.path === 'navigation.headingTrue' && number(value) !== null) {
            merged.heading = number(value) * DEGREES_PER_RADIAN;
          }
          if (update.timestamp) merged.time = update.timestamp;
        }
      }
      if (!validCoordinate(merged.lat, merged.lon)) return null;
      merged.quality = 'Signal K';
      return this.emit(merged, source, raw);
    }

    emit(candidate, source, raw) {
      const lat = number(candidate.lat);
      const lon = number(candidate.lon);
      if (!validCoordinate(lat, lon)) return null;
      const fix = {
        ...this.lastFix,
        ...this.motion,
        ...candidate,
        lat, lon,
        source: candidate.source || source || 'Unknown',
        raw,
        receivedAt: new Date().toISOString()
      };
      Object.keys(fix).forEach(key => {
        if (fix[key] === undefined) delete fix[key];
      });
      this.lastFix = fix;
      this.onFix(fix);
      return fix;
    }
  }

  return { Parser, nmeaCoordinate, checksumIsValid, validCoordinate };
});
