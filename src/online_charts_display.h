#ifndef ONLINE_CHARTS_DISPLAY_H_
#define ONLINE_CHARTS_DISPLAY_H_

// Display brightness in steps from day to a red night mode, as the GPS Charts
// extension's brightness button: Day, Dusk 75 %, Dusk 55 %, Dusk 40 %, Night.
// The plugin draws a filter over the whole chart in OpenCPN's last pass: the
// chart dimmed for the dusk steps, and for night inverted and turned red, which
// keeps night vision.

class wxDC;

namespace online_charts {

struct DisplayStep {
  const char* name;
  int scheme;         // OpenCPN colour scheme it needs (0 day), or -1 to leave it.
  double brightness;  // Multiplies the chart's colours (1 leaves them).
  bool red;           // Keeps only the red light (night).
  bool invert;        // Light chart areas turn dark and dark lines light first.
};

constexpr int kDisplaySteps = 5;
const DisplayStep& DisplayStepAt(int level);

// Draws the step's filter over the canvas (OpenGL, in the OVERLAY_OVER_UI pass).
void DrawDisplayFilterGl(int level);
// The wxDC canvas cannot multiply colours: it gets a dark veil for the same brightness.
void DrawDisplayFilterDc(wxDC& dc, int level, int width, int height);

}  // namespace online_charts

#endif  // ONLINE_CHARTS_DISPLAY_H_
