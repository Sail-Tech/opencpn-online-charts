#include "online_charts_quick_panel.h"

#include <algorithm>
#include <utility>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>
#include <wx/time.h>

namespace {

const int kPad = 12;
const int kControlHeight = 32;  // Comfortable for touch screens on board.
const int kTextWidth = 330;     // Long status lines wrap at this width.

wxStaticText* Heading(wxWindow* parent, wxSizer* sizer, const wxString& text) {
  auto* label = new wxStaticText(parent, wxID_ANY, text);
  wxFont font = label->GetFont();
  font.SetPointSize(std::max(8, font.GetPointSize() - 1));
  label->SetFont(font);
  label->SetForegroundColour(wxColour(110, 125, 132));
  sizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, kPad);
  return label;
}

wxBoxSizer* Row(wxWindow* parent, wxSizer* sizer, const wxString& label) {
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  row->Add(new wxStaticText(parent, wxID_ANY, label), 1, wxALIGN_CENTER_VERTICAL);
  sizer->Add(row, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, kPad);
  return row;
}

void Recolour(wxWindow* window, const wxColour& background, const wxColour& text) {
  // Native buttons keep their own look; panels, labels and check boxes follow the scheme.
  if (!wxDynamicCast(window, wxButton) && !wxDynamicCast(window, wxToggleButton)) {
    window->SetBackgroundColour(background);
    window->SetForegroundColour(text);
  }
  for (wxWindow* child : window->GetChildren()) Recolour(child, background, text);
}

}  // namespace

int OnlineChartsQuickPanel::Segments::Selected() const {
  for (size_t i = 0; i < buttons.size(); ++i) {
    if (buttons[i]->GetValue()) return static_cast<int>(i);
  }
  return 0;
}

void OnlineChartsQuickPanel::Segments::Select(int index) {
  for (size_t i = 0; i < buttons.size(); ++i) buttons[i]->SetValue(static_cast<int>(i) == index);
}

OnlineChartsQuickPanel::OnlineChartsQuickPanel(wxWindow* parent, const OnlineChartsSettings& settings,
                                               Actions actions)
    : wxFrame(parent, wxID_ANY, "Online Charts", wxDefaultPosition, wxDefaultSize,
              wxCAPTION | wxCLOSE_BOX | wxFRAME_TOOL_WINDOW | wxFRAME_FLOAT_ON_PARENT |
                  wxFRAME_NO_TASKBAR),
      settings_(settings),
      actions_(std::move(actions)) {
  panel_ = new wxPanel(this);
  auto* main = new wxBoxSizer(wxVERTICAL);

  enabled_ = new wxCheckBox(panel_, wxID_ANY, "Show Online Charts on the chart");
  wxFont bold = enabled_->GetFont();
  bold.SetWeight(wxFONTWEIGHT_BOLD);
  enabled_->SetFont(bold);
  main->Add(enabled_, 0, wxLEFT | wxRIGHT | wxTOP, kPad);
  enabled_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Apply(); });

  // The extension's brightness steps: OpenCPN's dusk scheme darkened further,
  // then a red night (online_charts_display.h).
  always_enabled_.push_back(Heading(panel_, main, "Display"));
  wxBoxSizer* brightness_row = Row(panel_, main, "Brightness");
  always_enabled_.push_back(brightness_row->GetItem(static_cast<size_t>(0))->GetWindow());
  brightness_ = AddSegments(panel_, brightness_row, {"Day", "75%", "55%", "40%", "Night"},
                            [this](int level) {
                              if (actions_.brightness) actions_.brightness(level);
                            },
                            52);

  Heading(panel_, main, "Depths");
  depth_ = AddSegments(panel_, Row(panel_, main, "Colours"), {"Off", "Chart", "Topo"});
  depth_opacity_ = AddSlider(panel_, main, "Opacity", &depth_opacity_value_);
  contours_ = new wxCheckBox(panel_, wxID_ANY, "Contours and soundings");
  main->Add(contours_, 0, wxLEFT | wxRIGHT | wxTOP, kPad);
  contours_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Apply(); });
  units_ = AddSegments(panel_, Row(panel_, main, "Units"), {"m", "ft"});

  Heading(panel_, main, "Base map (covers the OpenCPN chart)");
  base_map_ = AddSegments(panel_, Row(panel_, main, "Source"), {"Off", "Esri", "OSM"});
  base_opacity_ = AddSlider(panel_, main, "Opacity", &base_opacity_value_);
  seamarks_ = new wxCheckBox(panel_, wxID_ANY, "OpenSeaMap seamarks");
  main->Add(seamarks_, 0, wxLEFT | wxRIGHT | wxTOP, kPad);
  seamarks_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Apply(); });

  Heading(panel_, main, "AIS demo (testing only, not for navigation)");
  ais_ = new wxCheckBox(panel_, wxID_ANY, "Live vessels from Open Waters AIS");
  main->Add(ais_, 0, wxLEFT | wxRIGHT | wxTOP, kPad);
  ais_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Apply(); });
  ais_status_ = new wxStaticText(panel_, wxID_ANY, wxEmptyString);
  wxFont small = ais_status_->GetFont();
  small.SetPointSize(std::max(8, small.GetPointSize() - 1));
  ais_status_->SetFont(small);
  main->Add(ais_status_, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, kPad);

  main->Add(new wxStaticLine(panel_), 0, wxALL | wxEXPAND, kPad);
  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  // Only "More settings" (a modal dialog) closes the panel; the others act on the chart beside it.
  auto add_button = [&](const wxString& label, const wxString& tip, std::function<void()> action,
                        bool closes) {
    auto* button = new wxButton(panel_, wxID_ANY, label, wxDefaultPosition, wxSize(-1, kControlHeight));
    button->SetToolTip(tip);
    button->Bind(wxEVT_BUTTON, [this, action, closes](wxCommandEvent&) {
      if (closes) HidePanel();
      if (action) action();
    });
    buttons->Add(button, 1, wxRIGHT, 6);
  };
  add_button("Locate boat", "Centre the chart on the boat",
             [this]() {
               if (actions_.locate && !actions_.locate()) {
                 note_->SetLabel("OpenCPN has not reported the boat's position yet.");
                 note_->Show();
                 panel_->Layout();
                 Fit();
                 note_timer_.StartOnce(4000);
               }
             },
             false);
  add_button("Weather", "Show or hide the Weather window", actions_.weather, false);
  add_button("More settings", "Cache, labels, providers and weather settings", actions_.preferences, true);
  main->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, kPad);
  note_ = new wxStaticText(panel_, wxID_ANY, wxEmptyString);
  note_->Hide();
  main->Add(note_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, kPad);

  panel_->SetSizer(main);
  auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
  frame_sizer->Add(panel_, 1, wxEXPAND);
  SetSizer(frame_sizer);

  Sync(settings);
  Fit();
  SetMinSize(GetSize());

  // Close with Esc. It does not close when it loses the focus: OpenCPN's chart
  // takes the focus whenever the mouse or a finger moves over it.
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
    if (event.GetKeyCode() == WXK_ESCAPE) {
      HidePanel();
      return;
    }
    event.Skip();
  });
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    if (event.CanVeto()) {
      HidePanel();
      event.Veto();
    } else {
      event.Skip();
    }
  });

  status_timer_.SetOwner(this, wxID_HIGHEST + 41);
  apply_timer_.SetOwner(this, wxID_HIGHEST + 42);
  note_timer_.SetOwner(this, wxID_HIGHEST + 43);
  Bind(wxEVT_TIMER, [this](wxTimerEvent& event) {
    if (event.GetId() == apply_timer_.GetId()) {
      Apply();
    } else if (event.GetId() == note_timer_.GetId()) {
      note_->Hide();
      panel_->Layout();
      Fit();
    } else if (actions_.ais_status && IsShown()) {
      SetStatus(actions_.ais_status());
    }
  });
  status_timer_.Start(2000);
}

OnlineChartsQuickPanel::~OnlineChartsQuickPanel() {
  status_timer_.Stop();
  apply_timer_.Stop();
  note_timer_.Stop();
}

OnlineChartsQuickPanel::Segments OnlineChartsQuickPanel::AddSegments(
    wxWindow* parent, wxSizer* row, const std::vector<wxString>& labels,
    std::function<void(int)> on_select, int width) {
  Segments segments;
  for (size_t i = 0; i < labels.size(); ++i) {
    auto* button = new wxToggleButton(parent, wxID_ANY, labels[i], wxDefaultPosition,
                                      wxSize(width, kControlHeight));
    segments.buttons.push_back(button);
    row->Add(button, 0, wxALIGN_CENTER_VERTICAL);
  }
  // Pressing a button selects it; pressing the selected one keeps it selected.
  for (size_t i = 0; i < segments.buttons.size(); ++i) {
    std::vector<wxToggleButton*> all = segments.buttons;
    segments.buttons[i]->Bind(wxEVT_TOGGLEBUTTON, [this, all, i, on_select](wxCommandEvent&) {
      for (size_t j = 0; j < all.size(); ++j) all[j]->SetValue(j == i);
      if (on_select) {
        on_select(static_cast<int>(i));
      } else {
        Apply();
      }
    });
  }
  return segments;
}

wxSlider* OnlineChartsQuickPanel::AddSlider(wxWindow* parent, wxSizer* sizer, const wxString& label,
                                           wxStaticText** value) {
  auto* row = Row(parent, sizer, label);
  auto* slider = new wxSlider(parent, wxID_ANY, 70, 10, 100, wxDefaultPosition, wxSize(150, -1));
  slider->SetLineSize(5);
  slider->SetPageSize(10);
  row->Add(slider, 0, wxALIGN_CENTER_VERTICAL);
  *value = new wxStaticText(parent, wxID_ANY, "100%", wxDefaultPosition, wxSize(44, -1),
                            wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
  row->Add(*value, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
  wxStaticText* text = *value;
  slider->Bind(wxEVT_SLIDER, [this, slider, text](wxCommandEvent&) {
    text->SetLabel(wxString::Format("%d%%", slider->GetValue()));
    apply_timer_.StartOnce(150);
  });
  return slider;
}

void OnlineChartsQuickPanel::Sync(const OnlineChartsSettings& settings) {
  syncing_ = true;
  settings_ = settings;
  enabled_->SetValue(settings.overlay_enabled);
  depth_.Select(!settings.show_shading ? 0 : (settings.depth_colours == 1 ? 2 : 1));
  depth_opacity_->SetValue(settings.depth_opacity);
  depth_opacity_value_->SetLabel(wxString::Format("%d%%", settings.depth_opacity));
  contours_->SetValue(settings.show_contours);
  units_.Select(settings.depth_feet ? 1 : 0);
  base_map_.Select(!settings.show_base_map ? 0 : (settings.base_map_provider == 1 ? 1 : 2));
  base_opacity_->SetValue(settings.base_map_opacity);
  base_opacity_value_->SetLabel(wxString::Format("%d%%", settings.base_map_opacity));
  seamarks_->SetValue(settings.show_seamarks);
  ais_->SetValue(settings.ais_demo);
  if (actions_.ais_status) SetStatus(actions_.ais_status());
  UpdateEnabled();
  syncing_ = false;
}

void OnlineChartsQuickPanel::Apply() {
  if (syncing_) return;
  OnlineChartsSettings s = settings_;
  s.overlay_enabled = enabled_->GetValue();
  const int depth = depth_.Selected();
  s.show_shading = depth != 0;
  if (depth != 0) s.depth_colours = depth == 2 ? 1 : 0;
  s.depth_opacity = depth_opacity_->GetValue();
  s.show_contours = contours_->GetValue();
  s.show_soundings = contours_->GetValue();
  s.depth_feet = units_.Selected() == 1;
  const int base = base_map_.Selected();
  s.show_base_map = base != 0;
  if (base != 0) s.base_map_provider = base == 1 ? 1 : 0;
  s.base_map_opacity = base_opacity_->GetValue();
  s.show_seamarks = seamarks_->GetValue();
  s.ais_demo = ais_->GetValue();
  settings_ = s;
  UpdateEnabled();
  if (actions_.apply) actions_.apply(s);
  if (actions_.ais_status) SetStatus(actions_.ais_status());
}

void OnlineChartsQuickPanel::SetStatus(const wxString& text) {
  if (status_text_ == text) return;
  status_text_ = text;
  ais_status_->SetLabel(text);
  ais_status_->Wrap(kTextWidth);
  panel_->Layout();
  // A longer status may need more height; the width stays.
  const wxSize best = GetBestSize();
  if (best.y > GetClientSize().y) SetClientSize(GetClientSize().x, best.y);
}

void OnlineChartsQuickPanel::UpdateEnabled() {
  const bool on = enabled_->GetValue();
  for (wxWindow* child : panel_->GetChildren()) {
    if (child != enabled_ && !wxDynamicCast(child, wxButton)) child->Enable(on);
  }
  // The display brightness is OpenCPN's, not a layer of the plugin.
  for (auto* button : brightness_.buttons) button->Enable(true);
  for (auto* window : always_enabled_) window->Enable(true);
  depth_opacity_->Enable(on && depth_.Selected() != 0);
  base_opacity_->Enable(on && base_map_.Selected() != 0);
}

void OnlineChartsQuickPanel::SetColours(const wxColour& background, const wxColour& text) {
  if (!background.IsOk() || !text.IsOk()) return;
  Recolour(panel_, background, text);
  panel_->Refresh();
}

void OnlineChartsQuickPanel::SetBrightness(int level) {
  brightness_.Select(std::max(0, std::min(static_cast<int>(brightness_.buttons.size()) - 1, level)));
}

void OnlineChartsQuickPanel::HidePanel() {
  if (!IsShown()) return;
  if (apply_timer_.IsRunning()) {
    apply_timer_.Stop();
    Apply();
  }
  Hide();
}

