# Changelog

Release history of the OpenCPN Online Charts plugin, newest first. Build and validation notes for each release are in [BUILD_STATUS.md](BUILD_STATUS.md).

## 1.5.2 — Windows crash when enabling the plugin

**Windows.** OpenCPN crashed as soon as the plugin was enabled. OpenCPN 5.14.2 ships an old C++ runtime (`msvcp140.dll` 14.12, from Visual Studio 2017) next to `opencpn.exe`, and the plugin is built with Visual Studio 2022, whose `std::mutex` needs runtime 14.40 or later since version 17.10. The local dashboard server's first lock was an access violation inside `msvcp140.dll` (`_Mtx_lock`). The Windows build now defines `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`, Microsoft's documented fix for older runtimes. No other changes.

## 1.5.1 — Package for the 32-bit Windows OpenCPN

**Windows.** Version 1.5.0 had only a 64-bit Windows package (`msvc-64`). A 32-bit OpenCPN (`msvc-wx32`: every Windows OpenCPN from 5.8 to 5.14.0, and 32-bit installs of later versions) refused it with *Incompatible import plugin detected*. Each release now has both packages: `msvc-wx32-10-x86` for the 32-bit OpenCPN and `msvc-64-10-x86_64` for the 64-bit one. The 64-bit package's metadata now says `x86_64` instead of `x86`. No other changes.

## 1.5.0 — Garmin Marine and offline-area cache

**Garmin Marine.** **Open provider** has a third choice, **Open Garmin Marine**, which opens Garmin's English Marine page centred on the boat (or the chart centre without a GPS fix), as a 12-character geohash in the `key` parameter, the same URL as the GPS Charts extension 0.7.0. Only a click on Garmin's map places its marker, so the plugin opens Garmin in Google Chrome or Chromium when installed, with `gpscharts=pin` added, and the GPS Charts extension 0.7.1 or later places and checks the marker there. Without Chrome or the extension Garmin opens in the default browser and you click the map centre. The bundled browser dashboard lists Garmin too; it never opens Garmin at its demonstration position and does not reload Garmin to follow the boat, which would remove the marker. New `tests/geohash_test.cpp`.

**Offline cache.** The disk cache kept at most 4096 tiles, while **Save visible area for offline use** can save up to 5000, so a large saved area lost its oldest tiles as soon as the cache was trimmed. The cache now holds up to 20000 tiles, the tiles of the latest saved area are never trimmed, and a pass that starts trimming goes down to 90% of the limits instead of stopping just below them. Saving an area now reports its estimated size and refuses an area larger than the maximum cache size allows.

## 1.4.0 — Drawing speed, dashboard-sized labels and weather

**Drawing speed.** On a Retina Mac (2660 × 1446 canvas, 40 tiles) version 1.3.5 needed about **6 seconds per chart redraw**, which made panning and zooming unusable. Version 1.4.0 draws the same view in about **5 ms**. The causes and fixes:

- **Base map:** every raster tile was rescaled with `wxImage::Scale` and sent to the screen with `glDrawPixels` on every frame. Over-zoomed Esri tiles reached thousands of pixels. Each tile is now uploaded once as an OpenGL texture and drawn as a textured quad, with a parent tile magnified while a child loads. The wxDC canvas keeps a scaled bitmap until the drawn size changes.
- **Depth areas:** every polygon was tessellated by GLU on every frame. Each area is now tessellated once per tile, in tile coordinates, and drawn as a triangle array.
- **Projection:** every vertex cost a host `GetCanvasPixLL` call plus `atan`/`sinh`. A tile is now projected from a few sampled rows, which is exact for OpenCPN's Mercator view and keeps an exact per-point fallback for other projections.
- **Text:** a new `piDC` per frame rebuilt OpenCPN's texture font for each font change, and every label was drawn nine times for its halo. Labels now come from a glyph atlas built once, with mipmaps and a pre-computed halo, so any size draws in one pass.
- **Tile count:** zooms were measured in physical pixels, so a Retina canvas requested four times as many tiles. Vector tiles now follow the dashboard's 512-pixel styling in logical pixels (6 tiles instead of 40 in the view above), and raster tiles use logical pixels too.
- **Disk:** a missing tile no longer costs a file lookup on every frame while it downloads, and label collisions use a spatial grid.

**Labels.** The fixed 54 pt maximum is replaced by the dashboard's sizes: about 10 px when zoomed out, 12 px at the zoom above, growing to 18 px when zoomed in, scaled by **Depth label size (%)**. Soundings in metres show the decimetres as a smaller subscript digit, as in the dashboard. The old `DepthLabelSize` value is ignored.

**Contours.** Open Waters publishes a metric and a feet contour set, and the feet lines also carry metre values. Version 1.3.5 drew both sets in metres, doubling lines and showing values such as 1829, 914 or 366. Each unit now uses its own set, like the dashboard.

**Weather.** See [Weather](#weather): the Weather window and toolbar button, the forecast time bar on the chart, the wind and wave field on a cached lattice, the 72-hour cache and the handling of Open-Meteo's limits. The plugin now asks OpenCPN for position fixes (`WANTS_NMEA_EVENTS`) to place the wind barb at the boat, and for chart mouse events (`WANTS_MOUSE_EVENTS`) for the time bar and the field read-out.

**Exit crash.** The Weather window is deleted when the plugin unloads. `Destroy()` deferred its deletion until OpenCPN exited, after the plugin library had been unloaded, and OpenCPN then crashed on exit.

**Exit safety.** Constant colours are functions rather than global objects, so the library registers no exit-time destructors (`__cxa_atexit`), as required since 1.2.4.

Tested on an Intel Mac with OpenCPN 5.14.2: overlay timing, labels at two zooms, the weather panel and fields, all three preference tabs and a clean quit. The Apple Silicon slice was built but not run. Windows and Linux were not built here; the Linux build uses the new libcurl path in `online_charts_http.cpp`, which has not been compiled.

## 1.3.5 — MacOS zoom-responsive labels and collision avoidance

Version 1.3.5 replaces the fixed on-screen depth-label size with a smooth function of the viewport's precise Web Mercator zoom. The preference now defines the size at **zoom 15 and above**. Each zoom-out level multiplies the text size by **0.84**, with a 0.22 factor floor. With the default setting, this produces approximately 54 pt at z15, 45 pt at z14, 38 pt at z13, 32 pt at z12, 23 pt at z10 and 12–13 pt in distant views. The current calculated size appears in the preferences diagnostics.

The previous fixed 90×28/42×20 pixel occupancy grids are replaced by measured text rectangles with a five-pixel margin. Contours and soundings share one occupancy list, so labels from the two layers cannot overlap each other; a conflicting label is omitted. The 54 pt maximum, OpenCPN `ChartTexts` family, centering, halo, sounding italics and public raster base maps are preserved.

Both macOS slices were rebuilt and statically validated: 412 assigned wxWidgets imports checked per architecture, no missing exports, assert references, unexpected flat C++ imports or `__cxa_atexit` imports, macOS 11.0 targets and valid arm64 ad hoc signing. The clean archive passed metadata, path-safety, data-identity and binary-identity checks. Archive SHA-256: `beda8cba8c6b933a404bdb43e696efac300922c10870cab8f157c03bf584d610`. Actual dynamic appearance and label decluttering still require confirmation on the affected Mac.

## 1.3.4 — MacOS tripled labels and integrated raster base map

Version 1.3.4 triples the previous 18 pt depth-label default to **54 pt** and expands the persistent control to **10–96 pt**. A one-time configuration migration triples an existing 1.3.3 value, writes `DepthLabelSizeVersion=1`, and never multiplies it again on later starts. Contour and sounding labels retain the OpenCPN `ChartTexts` family, centering, halo and sounding italics.

The main chart now also supports a public raster reference map drawn below all bathymetric vectors. **Esri World Ocean Base** is the default; **OpenStreetMap** can be selected in native preferences. Base-map downloads are limited to visible tiles, with four concurrent requests and a bounded 128-image memory cache. They have no disk cache or prefetch and are not included in the bathymetry offline package. Esri uses global zoom-10 tiles overzoomed at closer view scales because detailed tiles are regional and z11+ can return HTTP 404. The chart includes the corresponding OpenStreetMap or Esri attribution.

Both macOS slices were rebuilt and statically validated: 411 assigned wxWidgets imports checked per architecture, no missing exports, assert references, unexpected flat C++ imports or `__cxa_atexit` imports, macOS 11.0 targets and valid arm64 ad hoc signing. The clean archive passed metadata, path-safety, data-identity and binary-identity checks. Archive SHA-256: `bf1be79484d3e037971d454300f249167fdab52c0e595fc9648843a9df0bd0db`. Actual raster placement, appearance and the preferred label size still require confirmation on the affected Mac.

## 1.3.3 — MacOS persistent depth-label size control

Version 1.3.3 addresses the confirmed limitation of 1.3.2: changing to `ChartTexts` and moving the labels did not make the numbers sufficiently large. Inspection of `piDC`/`TexFontPI` confirmed that the textured OpenGL path builds glyphs at the `wxFont` size; its `DrawTextEx()` scale argument does not scale those glyph quads. The plugin therefore copies the OpenCPN `ChartTexts` font, preserves its family and sounding italics, and explicitly applies a persistent **10–32 pt** size to contour and sounding labels. The default is **18 pt**, adjustable under **Options → Plugins → OnlineCharts → Preferences → Bathymetry label size (pt)**. Centred labels and the light halo are retained; diagnostics continue to use `StatusBar`.

Both macOS slices were rebuilt and statically validated: 406 assigned wxWidgets imports checked per architecture, no missing exports, assert references, unexpected flat C++ imports or `__cxa_atexit` imports, macOS 11.0 targets and valid arm64 ad hoc signing. The clean archive passed metadata, path-safety, data-identity and binary-identity checks. Archive SHA-256: `389f66e7cb254ea08bfe7ff33fe9d3d2951abe0bf9b6558c0f87fc07806c2742`. The actual visual improvement and preferred size still require confirmation on the affected Mac.

## 1.3.2 — MacOS OpenCPN-scaled depth labels

Version 1.3.2 replaces the fixed 8–10 point overlay fonts with OpenCPN's configured and GUI-scaled **ChartTexts** font for contours and soundings, and **StatusBar** for chart diagnostics. Depth values are centred on their contour or sounding position and have an eight-direction light halo for readability over chart colours. Soundings remain italic. The 1.3.1 native URLSession correction and all previous lifecycle fixes are preserved.

Both macOS slices were rebuilt and statically validated: 405 assigned wxWidgets imports checked per architecture, no missing exports, assert references or `__cxa_atexit` imports, macOS 11.0 targets and valid arm64 ad hoc signing. The clean archive passed metadata, path-safety, data-identity and binary-identity checks. Archive SHA-256: `9eba039e676b19707b2c01a3498a3dc25a1f985cd2397934d269a80cf240d697`. Actual label appearance still requires confirmation on the affected Mac.

## 1.3.1 — MacOS HTTPS session initialization fix

Version 1.3.1 fixes the failure reported by 1.3.0 on a real Mac: all tile attempts stopped synchronously at `CreateRequest()`, leaving 40 visible tiles, zero ready tiles and an empty cache. The overlay had default-constructed `wxWebSession`, although that constructor deliberately creates an invalid session. It now creates the session with `wxWebSession::New()`, checks `IsOpened()`, records whether the native `URLSession` backend is available and refuses to queue repeated per-tile failures if no session can be opened. The existing request-creation check remains as a secondary safeguard.

Both macOS slices were rebuilt with the previous release-wxWidgets, dynamic system C++ runtime and overlay-lifecycle fixes preserved. Static validation checked 404 assigned wxWidgets imports per architecture against OpenCPN 5.14.2 and found no missing exports, flat wxWidgets imports, assert references or `__cxa_atexit` imports. The clean archive passed metadata, path-safety, data-identity and binary-identity checks. Archive SHA-256: `7852af2824dcc2639c24610670dcbe955fb184f2b891da48425647f7eef2a5b6`. Actual HTTPS transfers and visible bathymetry still require confirmation on the affected Mac.

## 1.3.0 — MacOS preferences and bathymetry pipeline review

Version 1.3.0 adds the native persistent preferences dialog and configurable offline cache described above. The download queue now continues after each completed request rather than depending solely on another chart movement, HTTP 204 empty tiles are cached successfully, and stale cached data remains available after network failures. Cache clearing detaches active requests safely before deleting files.

The MVT parser retains metre and feet contours/soundings and filters alternative unit-tagged depth areas consistently with the reference dashboard. Contour labels, separate sounding/contour switches and source/render status were added to the integrated chart layer. The fallback multi-ring wxDC drawing helper was also corrected to use each ring's actual starting offset.

Both macOS slices were rebuilt with the release wxWidgets configuration and the dynamic system C++ runtime. Static validation checks the exact OpenCPN 5.14.2 wxWidgets exports, macOS 11.0 deployment target, universal architectures, entry points, ad hoc arm64 signature and absence of assert or exit-callback imports. The clean archive passed metadata, path-safety, data-identity and binary-identity checks. Archive SHA-256: `84edb2e394ca7c167a48f39f744f529a7201e696ddba084d8aafd1e79d7aa70c`. Real-Mac visual output and preferences behavior still require user confirmation; static validation cannot prove OpenGL pixels or native dialog interaction.

## 1.2.4 — MacOS overlay lifecycle and multi-canvas revision

Version 1.2.4 addresses the two issues reported after 1.2.3 loaded successfully: no visible bathymetry and an exit-time segmentation fault in `__cxa_finalize_ranges`. The macOS drawing library's global tessellation list no longer registers a process-exit destructor inside the unloadable plugin. Stream/locale formatting was removed, and the plugin now uses the SDK-matched dynamic system `libc++.1.dylib` and `libc++abi.dylib` rather than embedding a static C++ runtime with private exit callbacks.

The plugin now explicitly implements both legacy and API 1.18 multi-canvas overlay callbacks at `OVERLAY_LEGACY` priority. Its OpenGL path saves and restores host state, disables shaders, depth testing, face culling and textures for fixed-function drawing, enables alpha blending and reports a bounded number of OpenGL errors. No vessel coordinates are logged.

Static validation found zero `__cxa_atexit` imports, zero flat-namespace C++ imports, zero assert references and zero missing wxWidgets exports in both architectures. The clean-extracted archive passed path-safety, metadata, complete-data and binary-identity checks. Archive SHA-256: `5e59be8ca20635c6f4b1488fd4efc228ff6ab907c42b761dd2a322caa4db2a46`. Real-Mac rendering and clean shutdown still require confirmation.

## 1.2.3 — MacOS release-wxWidgets rebuild

Version 1.2.2 did **not** resolve the confirmed `_wxTheAssertHandler` load failure. OpenCPN 5.14.2 supplies release wxWidgets libraries that do not export the assert symbols referenced by the earlier debug-configured plugin. Version 1.2.3 is therefore a clean rebuild of the plugin and all drawing helpers with `wxDEBUG_LEVEL=0` and `NDEBUG`, linked against the exact eight wxWidgets libraries supplied by OpenCPN 5.14.2. It does not bundle separate wxWidgets copies.

Static validation checked 160 assigned wxWidgets imports in each architecture and found no missing exports, no flat-namespace wxWidgets imports and no assert references. Both slices export `create_pi`/`destroy_pi` and target macOS 11.0; the arm64 ad hoc signature has 61 valid SHA-256 code slots. The clean-extracted Import Plugin archive also passed path-safety, XML, content and binary-identity checks. Archive SHA-256: `8a54b6baf999e6ca3e6b5117ecbfaa5a9e66bb00f19357659c30e193d1137dd1`. Real-Mac installation, initialization and rendering still require confirmation.

## 1.2.1 — Diagnostics and safe replacement

The overlay writes bounded state-change messages prefixed `Online Charts:` to the OpenCPN log. They cover plugin `Init()`/`DeInit()`, HTTPS setup, request results, cache writes, viewport validity, derived tile zoom and the number of areas, contours and soundings actually drawn. They do not log vessel coordinates.

Before cancelling active `wxWebRequest` downloads during plugin replacement, version 1.2.1 detaches the request map. A synchronous cancellation callback can therefore no longer erase entries from the map being iterated. The macOS package also marks the plugin library executable (`0755`), matching official OpenCPN packages.

On macOS, the log is normally `~/Library/Logs/opencpn.log`; the **About** window also shows its actual location. If installation or replacement stalls, the last lifecycle message distinguishes extraction from old-plugin `DeInit()` and new-plugin `Init()`. If the toolbar is on but nothing appears, retain the recent `Online Charts:` lines. `visible_tiles=0` can indicate an invalid viewport or zoom below 4; failed requests indicate a network/HTTP problem; `ready_tiles=0` means no parsed data is available yet; positive `areas`, `contours` or `soundings` confirms that the drawing path was reached. These counters alone do not prove visual OpenGL output on macOS.

See `BUILD_STATUS.md` for the exact validation performed and platform limitations.
