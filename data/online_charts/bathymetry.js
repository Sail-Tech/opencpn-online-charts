'use strict';

const GPSBathymetry = (() => {
  const VECTOR_LAYERS = ['depth-areas', 'contour-lines', 'contour-labels', 'soundings'];

  function sources() {
    return {
      'seascape-vector': {
        type: 'vector',
        url: 'https://tiles.openwaters.io/seascape/vector.json',
        attribution: '© Open Waters · EMODnet · GEBCO'
      }
    };
  }

  function layers(config) {
    const visibility = config.bathymetryEnabled === false ? 'none' : 'visible';
    return [
      {
        id: 'depth-areas',
        type: 'fill',
        source: 'seascape-vector',
        'source-layer': 'depare',
        filter: ['!=', ['get', 'sys'], 'ft'],
        minzoom: 4,
        layout: {
          visibility,
          'fill-sort-key': ['get', 'rank']
        },
        paint: {
          'fill-color': [
            'case',
            ['!', ['has', 'drval1']], '#1f86cb',
            ['<', ['get', 'drval1'], 0], '#58af9c',
            [
              'step', ['get', 'drval1'],
              '#3fa2e4', 1.99, '#5db5f0', 4.99, '#7fc7f8',
              9.99, '#a5d9fb', 19.99, '#c9e9fd', 49.99, '#e9f7ff'
            ]
          ],
          'fill-opacity': 0.48
        }
      },
      {
        id: 'contour-lines',
        type: 'line',
        source: 'seascape-vector',
        'source-layer': 'contours',
        filter: ['!=', ['get', 'sys'], 'ft'],
        minzoom: 4,
        layout: {
          visibility,
          'line-join': 'round',
          'line-cap': 'round'
        },
        paint: {
          'line-color': ['case', ['==', ['get', 'depth_abs_m'], 10], '#4c5b63', '#657f8c'],
          'line-width': ['case', ['==', ['get', 'depth_abs_m'], 10], 1.6, 0.9],
          'line-opacity': 0.9
        }
      },
      {
        id: 'contour-labels',
        type: 'symbol',
        source: 'seascape-vector',
        'source-layer': 'contours',
        filter: ['!=', ['get', 'sys'], 'ft'],
        minzoom: 8,
        layout: {
          visibility,
          'symbol-placement': 'line',
          'text-field': ['to-string', ['get', 'depth_abs_m']],
          'text-size': ['interpolate', ['linear'], ['zoom'], 8, 12, 13, 16],
          'text-font': ['Noto Sans Medium'],
          'text-letter-spacing': 0.1,
          'text-max-angle': 30,
          'text-padding': 50
        },
        paint: {
          'text-color': '#4f6874',
          'text-halo-color': 'rgba(255,255,255,0.82)',
          'text-halo-width': 1
        }
      },
      {
        id: 'soundings',
        type: 'symbol',
        source: 'seascape-vector',
        'source-layer': 'soundings',
        minzoom: 7,
        layout: {
          visibility,
          'text-field': [
            'concat',
            ['to-string', ['floor', ['get', 'depth_m']]],
            [
              'match', ['round', ['*', 10, ['%', ['get', 'depth_m'], 1]]],
              1, ' ₁', 2, ' ₂', 3, ' ₃', 4, ' ₄', 5, ' ₅',
              6, ' ₆', 7, ' ₇', 8, ' ₈', 9, ' ₉', ''
            ]
          ],
          'text-font': ['Noto Sans Medium Italic'],
          'text-size': ['interpolate', ['linear'], ['zoom'], 8, 12, 13, 16],
          'text-padding': 8,
          'symbol-sort-key': [
            'case', ['==', ['get', 'prime'], 1], -1000000, ['get', 'depth_m']
          ]
        },
        paint: {
          'text-color': '#4f6874',
          'text-halo-color': 'rgba(255,255,255,0.82)',
          'text-halo-width': 1
        }
      }
    ];
  }

  function applyVisibility(map, config) {
    if (!map) return;
    const visibility = config.bathymetryEnabled === false ? 'none' : 'visible';
    for (const id of VECTOR_LAYERS) {
      if (map.getLayer(id)) map.setLayoutProperty(id, 'visibility', visibility);
    }
  }

  return { sources, layers, applyVisibility };
})();
