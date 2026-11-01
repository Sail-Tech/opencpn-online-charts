# Build and validation status

## Version 1.5.2 — Windows crash when enabling the plugin

The 1.5.1 `msvc-wx32` package imported in OpenCPN 5.14.2 (32-bit, Windows 10 22H2), then OpenCPN crashed when the plugin was enabled. The crash dump shows an access violation reading address 0 in `msvcp140.dll` 14.12.25810 (OpenCPN's own copy), called from `_Mtx_lock` in the plugin (`online_charts_pi.dll+0x97d4`, `OnlineChartsServer`'s mutex). Visual Studio 2022 17.10 made the `std::mutex` constructor constexpr, which requires runtime 14.40 or later. The Windows build now defines `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`.

## Version 1.5.1 — Package for the 32-bit Windows OpenCPN

The Windows CI job now builds two packages: `msvc-wx32` (Win32, wxWidgets 3.2.6) and `msvc-64` (x64, wxWidgets 3.3.4). OpenCPN's `PluginHandler::IsCompatible` accepts an `msvc*` package only when its target equals the host's own target: `msvc-wx32` for 32-bit builds (all of 5.8–5.14.0), `msvc-64` for 64-bit builds (5.14.2 and later; 5.14.0 had no 64-bit Windows build). Not yet checked: either Windows package imported and run in OpenCPN on Windows.

## Version 1.5.0 — Garmin Marine and offline-area cache

Built with `ci/build-macos-local.sh` and installed in OpenCPN 5.14.2 on an Intel Mac: the plugin loads and draws as before. The GitHub Actions build passes on macOS (universal), Debian 12 and Windows (VS 2022). Checked: the Garmin URL and geohash (`tests/geohash_test.cpp`, same reference values as the GPS Charts extension), the bundled dashboard's Garmin choice in a browser, and the GPS Charts extension placing and checking Garmin's marker on the live Garmin page. Not yet checked: the **Open Garmin Marine** menu item clicked inside OpenCPN (no synthetic clicks from this session), the Chrome lookup on Windows and Linux, and a full **Save visible area** of several thousand tiles against the new cache limits.

## Version 1.4.0 — drawing speed, dashboard-sized labels and weather

Measured in OpenCPN 5.14.2 on an Intel Retina Mac with the same coastal test view (2660 × 1446 canvas, OpenStreetMap base map), using a benchmark build that redraws the chart 300 times: version 1.3.5 needed about 6 s per redraw (40 vector tiles, 738 contours); 1.4.0 averages 4–5 ms (6 vector tiles, 91 contours, 109 soundings). See the README for the causes and fixes.

The universal library (x86-64 + arm64, macOS 11.0) was built with `ci/build-macos-local.sh` against OpenCPN's wxWidgets 3.2.4 headers and linked to the generic `@executable_path/../Frameworks/libwx_*-3.2.dylib` names. It has zero `__cxa_atexit` imports in both slices and a valid ad hoc signature. The x86-64 slice ran in OpenCPN with no warnings or errors in the log and a clean quit (no new crash report). The arm64 slice was not run.

Later in 1.4.0: a Weather window and toolbar button, a forecast time bar on the chart, a field read-out on hover, the field on a cached lattice, and Open-Meteo 429 handling. The forecast service answered *429 Daily API request limit exceeded* for this internet address during testing, so the window, the bar (play pressed through the plugin's mouse handler with the real cursor position: playing, +6 h, values updated), wave arrows on the lattice, per-point caching and the limit message were checked live, but wind barbs on the lattice could not be fetched; barbs themselves were seen earlier in the session. Real mouse clicks could not be sent from this session (no Accessibility permission), so clicking and dragging the bar by hand is not yet confirmed.

OpenSeaMap seamarks: The test OpenCPN had no chart folders configured, so the only chart symbols were the ones the browser dashboard draws from OpenSeaMap, a layer the plugin did not have. The plugin now draws `tiles.openseamap.org/seamark` tiles (transparent PNG, up to zoom 18) above the bathymetry, as uploaded textures. Checked live: channel lateral buoys, harbour light sectors, a cardinal buoy and marinas appear. Seamark tiles are network and memory only, like the base map, so they are not available offline. The forecast time bar was also seen playing after a real click.

Wind fallback and provider button: Open-Meteo's forecast service still answered *429 Daily API request limit exceeded*, so wind now falls back to MET Norway's Locationforecast. Checked live: field points cached with MET wind (`wind_met=1`), and standard wind barbs drawn across an estuary next to the seamarks and wave arrows. The new **Open provider** toolbar button is installed, but its menu and the browser opening were not clicked here (no Accessibility permission for synthetic clicks).

Ocean currents: red current arrows from Open-Meteo Marine (SMOC) were seen live on a European coast, and the window tile and chart panel read, for example, *Current 0.2 kn to SSW*. Field points over land (terrain above 2 m from MET Norway, 30–51 m in coastal towns) no longer get wave or current arrows; sea points (0 m) keep them.

Tide stations: OpenCPN 5.14.2's OpenGL canvas draws tide and current stations after the chart pass and before the legacy plugin pass (`gl_chart_canvas.cpp`), so the plugin's opaque base map covered them. Also, `RenderGLOverlay` and `RenderGLOverlayMultiCanvas` were both drawing, so everything was drawn twice. The chart layers now draw in the chart pass and weather in the legacy pass. Checked live: a harbour tide station icon shows above the base map, and drawing takes 6.5 ms per frame. *Show tides* was off in the OpenCPN configuration and was turned on for the test.

Checked at run time: overlay timing, contour labels at 12 px and 14 px at two zooms, metric contours without the feet set, the weather panel at the chart centre without a GPS, the wind and wave field, Latin-1 day names, the three preference tabs, weather readings reloaded from the disk cache, and a JSON reader test (`tests/json_test.cpp`) on real cached Open-Meteo responses under a comma-decimal locale. Not checked: the offline weather fallback after a failed request, the wind barb with a live GPS fix, the wxDC (non-OpenGL) canvas, and the Linux (libcurl) and Windows builds.

## Version 1.3.5 — macOS zoom-responsive labels and collision avoidance

The user confirmed that the fixed 54 pt size was appropriate at maximum zoom but too large when zooming out. Version 1.3.5 calculates text size from the precise viewport zoom rather than the integer tile zoom. The saved preference is now the maximum size at z15+. The multiplier is `clamp(0.84 ^ max(0, 15 - zoom), 0.22, 1.0)`, rounded to whole points with a 10 pt minimum. At the default setting this yields 54 pt at z15, 45 pt at z14, 38 pt at z13, 32 pt at z12, 27 pt at z11, 23 pt at z10 and 12–13 pt at distant zooms. Preferences expose the calculated current size.

Fixed grid-cell label suppression was insufficient for the larger text. Both contour and sounding labels now measure their actual text extents, add a five-pixel margin and test a shared vector of occupied `wxRect` rectangles. Conflicting labels are not drawn. This also prevents cross-layer contour/sounding overlap. The explicit font sizing, centering, halo, italics, public base maps, URLSession and cache behavior remain unchanged.

Both architectures compiled successfully. The universal library is 1,306,464 bytes (640,307-byte x86-64 slice and 634,720-byte arm64 slice). Static validation checked 412 assigned wxWidgets imports per architecture against OpenCPN 5.14.2 and found zero missing exports, zero flat wxWidgets imports, zero assert references, zero `__cxa_atexit` imports and zero unexpected flat C++ imports. Both slices target macOS 11.0; the arm64 ad hoc signature has 39 valid SHA-256 code slots.

The clean-extracted Import Plugin archive has one safe root, 34 entries, 20 byte-identical data files, library mode `0755`, API 1.18/version 1.3.5 metadata and a byte-identical validated universal binary. Archive size: 893,050 bytes. Archive SHA-256: `beda8cba8c6b933a404bdb43e696efac300922c10870cab8f157c03bf584d610`.

No real Mac runtime is available here. Static validation cannot prove final OpenGL pixels or the visual density selected by collision avoidance. The dynamic size and label placement must be confirmed on the affected Mac. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.3.4 — macOS tripled labels and integrated public base maps

This revision triples the previous depth-label default from 18 pt to 54 pt and expands the native persistent control to 10–96 pt. A one-time `DepthLabelSizeVersion` migration triples an existing 1.3.3 value and persists the result, preventing repeated multiplication. The explicit `SetPointSize()` textured-font correction, centering, halo and sounding italics are preserved.

A new `OnlineChartsLandLayer` draws public raster tiles below the Open Waters vectors. Native preferences provide an enable switch and an OpenStreetMap/Esri World Ocean Base selector, with Esri as the default. The layer uses native `wxWebSession::New()` HTTPS, at most four concurrent visible-area requests and a 128-image LRU memory cache. It intentionally has no disk cache or prefetch. Esri is capped at its globally available zoom 10 and overzoomed at closer scales, avoiding blank results where regional z11+ tiles return HTTP 404. A real European zoom-10 Esri tile returned HTTP 200 and decoded as a 256×256 JPEG. OpenStreetMap could not be reached from this build environment, so its native transfer remains unconfirmed; it is selectable rather than the default.

Both architectures compiled successfully. The universal library is 1,306,128 bytes (639,908-byte x86-64 slice and 634,384-byte arm64 slice). Static validation checked 411 assigned wxWidgets imports per architecture against OpenCPN 5.14.2 and found zero missing exports, zero flat wxWidgets imports, zero assert references, zero `__cxa_atexit` imports and zero unexpected flat C++ imports. Explicit font sizing, native HTTPS and raster image decoding imports are present. Both slices target macOS 11.0; the arm64 ad hoc signature has 39 valid SHA-256 code slots.

The clean-extracted Import Plugin archive has one safe root, 34 entries, 20 byte-identical data files, library mode `0755`, API 1.18/version 1.3.4 metadata and a byte-identical validated universal binary. Archive size: 892,785 bytes. Archive SHA-256: `bf1be79484d3e037971d454300f249167fdab52c0e595fc9648843a9df0bd0db`.

No real Mac runtime is available here. Static validation and image decoding cannot prove final OpenGL pixels, raster alignment or native dialog behavior. The user must confirm the base map and 54 pt labels on the affected Mac. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.3.3 — macOS persistent bathymetry label sizing

The user confirmed that 1.3.2 changed label placement but left the depth numbers too small. Source inspection found that `piDC::SetFont()` stores the supplied `wxFont`, `TexFontPI::Build()` rasterizes that font into an OpenGL texture and renders native-sized glyph quads, and the texture-path `DrawTextEx()` scale argument affects background dimensions but not glyph size. Version 1.3.3 therefore calls `SetPointSize()` on a private copy of OpenCPN's `ChartTexts` font. A new native **Bathymetry label size (pt)** spin control applies the same explicit size to contours and italic soundings. It accepts 10–32 pt, defaults to 18 pt and persists as `DepthLabelSize` below `/PlugIns/OnlineCharts`. Status text remains on OpenCPN's `StatusBar` font. Existing URLSession, cache, centred-label, halo and lifecycle behavior is preserved.

Both architectures compiled cleanly except for the existing informational `wxUSE_GRAPHICS_CONTEXT` pragma. The universal library is 1,176,416 bytes (579,179-byte x86-64 slice and 570,208-byte arm64 slice). Static validation checked 406 assigned wxWidgets imports per architecture against OpenCPN 5.14.2 and found zero missing exports, zero flat wxWidgets imports, zero assert references, zero `__cxa_atexit` imports and zero unexpected flat C++ imports. The explicit `SetPointSize`, scaled-font and native HTTPS-session imports are present in both slices. Both slices target macOS 11.0; the arm64 ad hoc signature has 35 valid SHA-256 code slots.

The clean-extracted Import Plugin archive has one safe root, 34 entries, 20 byte-identical data files, library mode `0755`, API 1.18/version 1.3.3 metadata and a byte-identical validated universal binary. Archive size: 844,975 bytes. Archive SHA-256: `389f66e7cb254ea08bfe7ff33fe9d3d2951abe0bf9b6558c0f87fc07806c2742`.

No real Mac runtime is available here. Static checks establish that explicit font sizing is compiled and linked, but cannot prove the resulting OpenGL pixels. The user should confirm the 18 pt default and adjust it if necessary. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.3.2 — macOS OpenCPN-scaled bathymetry labels

The user confirmed that 1.3.1 downloads and draws bathymetric contours on the main chart. The remaining native issue was tiny depth text produced by fixed 8–10 point fonts. Version 1.3.2 calls `GetOCPNScaledFont_PlugIn("ChartTexts", 10)` for contours and soundings, preserving italic soundings, and uses the configured `StatusBar` font for diagnostics. Labels are centred using `piDC::GetTextExtent()` and receive an eight-direction light halo. No fixed zoom-dependent font size remains.

Both architectures compiled cleanly except for the existing informational `wxUSE_GRAPHICS_CONTEXT` pragma. Static validation checked 405 assigned wxWidgets imports per architecture against OpenCPN 5.14.2 and found zero missing exports, zero flat wxWidgets imports, zero assert references, zero `__cxa_atexit` imports and zero unexpected flat C++ imports. The native HTTPS-session checks remain linked, and the new OpenCPN scaled-font host import is present in both slices. Both slices target macOS 11.0; the arm64 ad hoc signature has 35 valid SHA-256 code slots.

The clean-extracted Import Plugin archive has one safe root, 34 entries, 20 byte-identical data files, library mode `0755`, API 1.18/version 1.3.2 metadata and a byte-identical validated universal binary. Archive size: 843,445 bytes. Archive SHA-256: `9eba039e676b19707b2c01a3498a3dc25a1f985cd2397934d269a80cf240d697`.

No real Mac runtime is available here. The configured font size, halo, label placement and clean shutdown must still be confirmed on the affected Mac. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.3.1 — macOS native URLSession initialization fix

The real-Mac 1.3.0 diagnostics showed 40 visible tiles, zero ready tiles, zero active/queued requests, zero successful downloads and 259 failures at request creation. The direct cause was an invalid default-constructed `wxWebSession`: the wxWidgets API explicitly requires the `New()` factory to create a usable session. Version 1.3.1 initializes the member with `wxWebSession::New()`, checks `IsOpened()` and logs `URLSession_available` plus `session_ready`. A missing session now produces one clear status error instead of incrementing a failure for every visible tile. Request validation remains in place, and the User-Agent identifies version 1.3.1.

Both architectures compiled cleanly except for the existing informational `wxUSE_GRAPHICS_CONTEXT` pragma. The universal library is 1,175,904 bytes (578,677-byte x86-64 slice and 569,696-byte arm64 slice). Static Mach-O validation checked 404 assigned wxWidgets imports per architecture against OpenCPN 5.14.2 and found zero missing exports, zero flat wxWidgets imports, zero assert references, zero `__cxa_atexit` imports and zero unexpected flat C++ imports. Both slices export the entry points, target macOS 11.0 and import the session factory, backend-availability and session-state checks. The arm64 ad hoc signature has 35 valid SHA-256 code slots.

The clean-extracted Import Plugin archive has one safe root, 34 entries, 20 byte-identical data files, library mode `0755`, API 1.18/version 1.3.1 metadata and a byte-identical validated universal binary. Archive size: 842,681 bytes. Archive SHA-256: `7852af2824dcc2639c24610670dcbe955fb184f2b891da48425647f7eef2a5b6`.

No real Mac runtime is available here. Static checks establish that the corrected session API is present and linked, but successful HTTPS downloads, parsed tile counts, visible bathymetry and clean shutdown must still be confirmed on the affected Mac. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.3.0 — macOS native preferences and bathymetry pipeline review

This revision adds `WANTS_PREFERENCES` and a native `ShowPreferencesDialog()` implementation. Persistent settings are stored through `GetOCPNConfigObject()` under `/PlugIns/OnlineCharts`. The dialog includes layer visibility, metres/feet, status display, cache modes/limits, visible-area prefetch, cache clearing and live source/render counters. Browser-only GPS, provider and weather settings are deliberately not duplicated in the OpenCPN plugin.

The bathymetry review covered viewport zoom/ranges, asynchronous request ownership, queue progress, HTTP 204 responses, gzip decoding, real MVT parsing, disk freshness/fallback, cache maintenance, unit-specific properties, ring offsets, contour/sounding geometry and wxDC/OpenGL state. Real public tiles covering three European coastal areas at zooms 4–15 were parsed successfully after the changes. Metric and feet fields were independently verified in representative tiles. Empty tiles now parse and cache as valid rather than retrying as failures.

Both macOS architectures compiled cleanly except for the existing informational `wxUSE_GRAPHICS_CONTEXT` pragma. Static Mach-O validation checked 401 assigned wxWidgets imports per architecture against OpenCPN 5.14.2 and found zero missing exports, zero flat wxWidgets imports, zero assert references and zero undefined `__cxa_atexit`/flat C++ imports. Both slices export the entry points and target macOS 11.0; the arm64 ad hoc signature has 35 valid SHA-256 code slots. The clean-extracted archive has one safe root, 34 entries, mode `0755` for the library and 20 byte-identical data files. Archive SHA-256: `84edb2e394ca7c167a48f39f744f529a7201e696ddba084d8aafd1e79d7aa70c`.

No real Mac runtime is available here. The new chart status and preference counters are intended to make the remaining display issue observable, but successful native dialog behavior and visible pixels still require confirmation on the affected Mac. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.2.4 — macOS 11+ universal overlay lifecycle and multi-canvas revision

After 1.2.3 loaded successfully, the affected Intel Mac reported absent bathymetry and an exit-time `EXC_BAD_ACCESS` in `exit → __cxa_finalize_ranges`, calling an unmapped address. This is consistent with a process-exit callback belonging to an unloaded plugin. Static inspection found a global `wxArrayPtrVoid` destructor in `pidc.cpp` and additional private libc++ locale exit callbacks introduced by the cross-compiler's statically linked C++ runtime.

Version 1.2.4 marks the macOS tessellation-list global `[[clang::no_destroy]]`, removes stream/locale formatting used for JSON, dates, labels and diagnostics, compiles against the MacOSX11.3.sdk C++ headers and dynamically links `/usr/lib/libc++.1.dylib` and `/usr/lib/libc++abi.dylib`. It no longer embeds Zig's static libc++. Both architectures have zero undefined `__cxa_atexit` imports and zero flat-namespace C++ imports.

The overlay now explicitly implements legacy and API 1.18 four-argument multi-canvas callbacks, drawing only at `OVERLAY_LEGACY` priority. The OpenGL path saves/restores host attributes and the current shader, disables depth testing, face culling and textures for compatibility fixed-function drawing, disables depth writes, enables alpha blending and logs up to eight OpenGL errors without vessel coordinates.

The universal binary passed clean extraction and repeated Mach-O validation: 160 assigned wxWidgets imports checked per architecture, zero missing exports, zero flat wxWidgets imports, zero assert references, API 1.18 entry points, macOS 11.0 deployment targets and 28 valid arm64 ad hoc SHA-256 code slots. The archive contains one safe root, 34 entries, mode `0755` for the native library and all 20 data files byte-identical to the source data. Archive SHA-256: `5e59be8ca20635c6f4b1488fd4efc228ff6ab907c42b761dd2a322caa4db2a46`.

No real Mac runtime is available here. These changes directly address the observed exit-callback failure and strengthen both overlay dispatch paths, but visible bathymetry and clean shutdown must still be confirmed on the affected Mac. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.2.3 — macOS 11+ universal release-wxWidgets rebuild

The affected Intel Mac reported `dlopen(...): symbol not found in flat namespace '_wxTheAssertHandler'`. Version 1.2.2 did not fix that failure: adding wxWidgets load commands did not correct the symbol ordinals, and the release wxWidgets 3.2.0.4.2 libraries supplied by OpenCPN 5.14.2 do not export the referenced assert symbols.

Version 1.2.3 is a clean compilation, not a binary patch. The plugin and all `plugin_dc` drawing helpers are compiled with `wxDEBUG_LEVEL=0` and `NDEBUG`, then linked against the exact eight wxWidgets libraries extracted from the official OpenCPN 5.14.2 universal package. The plugin uses OpenCPN's runtime libraries and does not bundle separate wxWidgets copies. The permanent macOS build configuration applies the release-wxWidgets definitions to all relevant targets.

Static Mach-O validation passed independently for x86-64 and arm64. In each slice, 160 assigned wxWidgets imports were checked against the corresponding real library exports, with zero missing exports, zero flat-namespace wxWidgets imports and zero assert references. Both slices export `create_pi` and `destroy_pi`, use `@rpath/libonline_charts_pi.dylib` and specify macOS 11.0 as their minimum deployment target. The arm64 ad hoc signature has 61 valid SHA-256 code slots.

The Import Plugin archive was extracted into a clean directory and validated: gzip/tar integrity, one expected top-level directory, no absolute paths, traversal entries, links or device entries, parseable API 1.18/version 1.2.3 metadata, `darwin-wx32` target version 10, both declared architectures, archive library mode `0755`, all 20 expected data files and exact SHA-256 identity with the validated universal binary. The extracted binary independently passed the same Mach-O validation. Archive SHA-256: `8a54b6baf999e6ca3e6b5117ecbfaa5a9e66bb00f19357659c30e193d1137dd1`.

The clean 1.2.3 library has not been loaded on a real Mac. Static validation specifically addresses the confirmed load failure, but installation, initialization, activation and bathymetric rendering still require confirmation on the affected system. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.2.2 — superseded macOS linkage attempt

Version 1.2.2 added eight explicit wxWidgets library references but retained the incompatible assert references. It is superseded by 1.2.3 and must not be represented as a fix for `_wxTheAssertHandler`.

## Version 1.2.1 — macOS 11+ universal installation revision

This macOS-focused revision addresses a possible freeze while replacing an active plugin. The previous shutdown path iterated over the request map while calling `wxWebRequest::Cancel()`. A synchronous cancellation event could call `CompleteDownload()` and erase from that same map. Version 1.2.1 first detaches the map, so cancellation callbacks find no active request entries and cannot invalidate the shutdown loop.

The package sets `libonline_charts_pi.dylib` to mode `0755`, matching official macOS plugin packages; version 1.2.0 used `0644`. Additional bounded messages identify the start and completion of `Init()` and `DeInit()`, overlay shutdown, request results and drawing state. No vessel coordinates are logged.

The 1.2.1 archive was built and statically validated: gzip/tar integrity, safe relative paths, API 1.18 metadata, both x86-64 and arm64 slices, macOS 11.0 deployment targets, `@rpath/libonline_charts_pi.dylib`, `create_pi`/`destroy_pi` exports, system-only dependencies and valid arm64 ad hoc code-directory hashes. Archive SHA-256: `4503f8a2f90430d4f671f40f9bade80701f2a3bb066df365ceaa6136c057c6cd`.

The installation freeze has not been reproduced on a real Mac. The revised shutdown logic and archive structure can be validated here, but successful installation and rendering must still be confirmed on the affected macOS system. Ad hoc arm64 signing is not Developer ID signing or Apple notarization.

## Version 1.2.0 — Kali Linux rolling x86-64

The Kali package contains a native `libonline_charts_pi.so` using OpenCPN plugin API 1.18. It was compiled against the current Kali dependency set used for the test, including wxWidgets 3.2.11, libcurl 8.22.0 and OpenCPN 5.14.2. Both required entry points, `create_pi` and `destroy_pi`, are exported. The library has no embedded build-directory RPATH or RUNPATH.

The compatibility failure of the earlier Debian package was reproduced and identified: that archive declared `debian-x86_64` / `12`, whereas the Kali OpenCPN runtime reported `debian-x86_64` / `n/a`. The 1.2.0 Kali archive declares the latter pair in `metadata.xml`.

The real **Options → Plugins → Import plugin…** dialog accepted the revised `n/a` metadata and displayed **“PluginOnlineCharts successfully imported”**. The first isolated container run could not install files because OpenCPN itself reported `PluginPaths: Unknown platform`; this was a container platform-detection failure, not evidence of successful plugin loading. A revised test setup removed that platform-detection warning. Full installation, native loading and visual overlay validation in Kali are not yet confirmed. The package must therefore be treated as a compatibility build requiring an on-host Kali test before navigational use.

## Version 1.2.0 — macOS 11+ universal

The macOS archive contains `metadata.xml`, the complete auxiliary dashboard data and a universal `libonline_charts_pi.dylib` with Intel x86-64 and Apple Silicon arm64 slices. Both slices export `create_pi` and `destroy_pi` and specify macOS 11.0 as their minimum deployment target. The arm64 slice carries the ad hoc signature required by Apple Silicon. Ad hoc signing is not Developer ID signing or Apple notarization.

This revision starts the stored `wxWebRequest` object after it reaches stable request-map storage and adds bounded network/render diagnostics. It does not establish that the reported macOS display problem is fixed. The package has been structurally validated but has not been imported, launched or visually tested in a real macOS runtime. Testing on the affected Mac remains necessary.

The log normally resides at `~/Library/Logs/opencpn.log`; OpenCPN's **About** window shows the actual path. Recent lines prefixed `Online Charts:` identify the HTTPS backend, request/parse/cache outcomes, viewport validity, derived zoom, visible/ready tile counts and objects submitted for drawing. No vessel coordinates are included in these messages.

## Version 1.1.0 — Linux x86-64 / Debian 12 reference test

The earlier 1.1.0 native Debian 12 build completed the real Import Plugin workflow in isolated OpenCPN 5.14.2. The library loaded, the plugin enabled and its check-style toolbar button appeared.

The integrated overlay was visually tested over a European coastal area at chart scale **1:77000**. Open Waters depth areas, contours, numeric soundings and attribution were drawn directly on the main chart. Twelve real zoom-12 PBF tiles were downloaded and written to the native flat-file cache. The toolbar button was tested off and on.

OpenCPN was then restarted with network isolation. The same bathymetry rendered entirely from the existing cache, without changes to cached files. This validates offline reuse of previously viewed tiles only.

That graphical test used the wxDC path with `OpenGL=0`. OpenGL drawing code compiled, but visual OpenGL output was not validated. The new 1.2.0 diagnostic changes have not replaced that earlier visual test with a new Debian runtime test.

## Windows

GitHub Actions builds a 32-bit (`msvc-wx32`, x86) and a 64-bit (`msvc-64`, x86_64) package with Visual Studio 2022 (`.github/workflows/build.yml`, `ci/build-windows.bat` with `PLATFORM=Win32` or `x64`). Neither has been run in OpenCPN on Windows from this environment. Platform tarballs are not interchangeable, and the Windows package must match OpenCPN's own architecture, not Windows'.

## Runtime and safety limitations

Only public Open Waters vectors are used for the integrated layer. Protected C-MAP and i-Boating endpoints are not bypassed. The auxiliary local service listens on loopback and must not be exposed publicly without authentication. C-MAP Pin Location does not automatically save a pin or create a route.

No live GPS receiver was connected. Earlier loopback HTTP/WebSocket tests validated only the auxiliary service.

This is an experimental chart display, not an ENC or a primary navigation source. Test the exact OpenCPN release and operating system before use.
