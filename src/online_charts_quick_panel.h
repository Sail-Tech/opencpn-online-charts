#ifndef ONLINE_CHARTS_QUICK_PANEL_H_
#define ONLINE_CHARTS_QUICK_PANEL_H_

#include <functional>
#include <vector>

#include <wx/colour.h>
#include <wx/frame.h>
#include <wx/string.h>
#include <wx/timer.h>

#include "online_charts_overlay.h"

class wxCheckBox;
class wxPanel;
class wxSizer;
class wxSlider;
class wxStaticText;
class wxToggleButton;
class wxWindow;

// The options used while sailing, one click from the toolbar button: display
// brightness (OpenCPN's day, dusk and night), layers, depth colours and their
// opacity, units, base map, seamarks and the AIS demo. Every change applies at
// once, so its effect shows on the chart behind the panel. Buttons locate the
// boat and open the Weather window and the full preferences. The panel stays
// open while the chart is used, and closes with its button, Esc or the toolbar.
class OnlineChartsQuickPanel : public wxFrame {
 public:
  struct Actions {
    std::function<void(const OnlineChartsSettings&)> apply;
    std::function<void()> weather;      // Show or hide the Weather window.
    std::function<bool()> locate;       // Centre the chart on the boat; false without a position.
    std::function<void()> preferences;  // Open the full preferences dialog.
    std::function<void(int)> brightness;  // 0 day, 1-3 dusk 75/55/40 %, 4 red night.
    std::function<wxString()> ais_status;
  };

  OnlineChartsQuickPanel(wxWindow* parent, const OnlineChartsSettings& settings, Actions actions);
  ~OnlineChartsQuickPanel() override;

  // Copies the settings into the controls (after the preferences dialog or the toolbar).
  void Sync(const OnlineChartsSettings& settings);
  // OpenCPN's day, dusk and night colours, and the display step in use (0-4).
  void SetColours(const wxColour& background, const wxColour& text);
  void SetBrightness(int level);

 private:
  // A row of toggle buttons of which exactly one is pressed.
  struct Segments {
    std::vector<wxToggleButton*> buttons;
    int Selected() const;
    void Select(int index);
  };

  // on_select runs when a segment is chosen; without it the settings are applied.
  Segments AddSegments(wxWindow* parent, wxSizer* row, const std::vector<wxString>& labels,
                       std::function<void(int)> on_select = nullptr, int width = 64);
  wxSlider* AddSlider(wxWindow* parent, wxSizer* sizer, const wxString& label, wxStaticText** value);
  void Apply();
  void UpdateEnabled();
  void HidePanel();
  void SetStatus(const wxString& text);

  OnlineChartsSettings settings_;
  Actions actions_;
  wxPanel* panel_ = nullptr;
  wxCheckBox* enabled_ = nullptr;
  Segments brightness_;
  std::vector<wxWindow*> always_enabled_;  // The display rows, which are not plugin layers.
  Segments depth_;
  wxSlider* depth_opacity_ = nullptr;
  wxStaticText* depth_opacity_value_ = nullptr;
  wxCheckBox* contours_ = nullptr;
  Segments units_;
  Segments base_map_;
  wxSlider* base_opacity_ = nullptr;
  wxStaticText* base_opacity_value_ = nullptr;
  wxCheckBox* seamarks_ = nullptr;
  wxCheckBox* ais_ = nullptr;
  wxStaticText* ais_status_ = nullptr;
  wxString status_text_;
  wxStaticText* note_ = nullptr;  // A short message under the buttons, such as "no position yet".
  wxTimer note_timer_;
  wxTimer status_timer_;
  wxTimer apply_timer_;  // Sliders apply once they rest, not on every step.
  bool syncing_ = false;
};

#endif  // ONLINE_CHARTS_QUICK_PANEL_H_
