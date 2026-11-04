#include "online_charts_display.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>

// ocpn_plugin.h first: through wxWidgets it brings in windows.h, which the
// OpenGL header in pidc.h needs on Windows (WINGDIAPI, APIENTRY).
#include "ocpn_plugin.h"
#include "online_charts_render.h"
#include "pidc.h"

namespace online_charts {

namespace {

// The extension's steps: the chart dimmed to 75, 55 and 40 %, then a red night.
// Night inverts a light chart (dark lines become the light ones, as in the
// extension) and keeps only the red: it needs OpenCPN's day palette, since
// its dusk and night palettes already have a dark background.
const DisplayStep kSteps[kDisplaySteps] = {
    {"Day", -1, 1.00, false, false},
    {"Dusk 75%", -1, 0.75, false, false},
    {"Dusk 55%", -1, 0.55, false, false},
    {"Dusk 40%", -1, 0.40, false, false},
    {"Night", 0, 0.95, true, true},
};

}  // namespace

namespace {

void Quad() {
  // Larger than any canvas, in canvas pixels.
  glBegin(GL_QUADS);
  glVertex2f(-16.0f, -16.0f);
  glVertex2f(16384.0f, -16.0f);
  glVertex2f(16384.0f, 16384.0f);
  glVertex2f(-16.0f, 16384.0f);
  glEnd();
}

}  // namespace

const DisplayStep& DisplayStepAt(int level) {
  return kSteps[std::max(0, std::min(kDisplaySteps - 1, level))];
}

void DrawDisplayFilterGl(int level) {
  const DisplayStep& step = DisplayStepAt(level);
  if (!step.red && !step.invert && step.brightness >= 0.999) return;
  glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT |
               GL_DEPTH_BUFFER_BIT);
  const int program = SaveAndClearProgram();
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  if (step.invert) {
    // destination = 1 - destination
    glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    Quad();
  }
  if (step.red) {
    // destination = destination x destination: more contrast, so the background
    // goes nearly black while the lines stay bright (the extension's contrast).
    glBlendFunc(GL_ZERO, GL_DST_COLOR);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    Quad();
  }
  // destination = destination x colour: darker, and red only at night.
  glBlendFunc(GL_DST_COLOR, GL_ZERO);
  const float b = static_cast<float>(step.brightness);
  if (step.red) {
    glColor4f(b, 0.10f * b, 0.12f * b, 1.0f);
  } else {
    glColor4f(b, b, b, 1.0f);
  }
  Quad();
  RestoreProgram(program);
  glPopAttrib();
}

void DrawDisplayFilterDc(wxDC& dc, int level, int width, int height) {
  const DisplayStep& step = DisplayStepAt(level);
  if (!step.red && step.brightness >= 0.999) return;
  wxWindowDC* window_dc = wxDynamicCast(&dc, wxWindowDC);
  wxMemoryDC* memory_dc = wxDynamicCast(&dc, wxMemoryDC);
  if (!window_dc && !memory_dc) return;
  std::unique_ptr<wxGCDC> gc(window_dc ? new wxGCDC(*window_dc) : new wxGCDC(*memory_dc));
  // A veil: black for the dusk steps, dark red at night.
  const int alpha = step.red ? 150 : static_cast<int>((1.0 - step.brightness) * 255.0);
  gc->SetPen(*wxTRANSPARENT_PEN);
  gc->SetBrush(wxBrush(step.red ? wxColour(60, 0, 0, alpha) : wxColour(0, 0, 0, alpha)));
  gc->DrawRectangle(0, 0, width, height);
}

}  // namespace online_charts
