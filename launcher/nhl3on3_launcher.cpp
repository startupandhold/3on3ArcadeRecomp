// NHL 3 on 3 launcher for Linux (SDL3 + Dear ImGui), the counterpart of
// nhl3on3_launcher.ps1 on Windows: edits nhl3on3.toml beside the nhl3on3
// binary and starts the game. Lines in the file that the launcher doesn't
// manage are kept as they are, and settings left at their defaults are
// omitted from the file. Keep the settings in step with the PowerShell one.

#include <SDL3/SDL.h>
#include <unistd.h>

#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "nhl3on3_launcher_window.h"

namespace fs = std::filesystem;

namespace {

// Bool: checkbox. Choice: fixed list. Edit: list or typed value. Text: typed
// value. Size: a window_width / window_height pair chosen from a list.
enum class Kind { Bool, Choice, Edit, Text, Size };
enum class Toml { Bool, Int, String };

struct Setting {
  const char* tab;
  const char* key;
  const char* label;
  Kind kind;
  Toml toml;
  const char* default_value;
  std::vector<std::pair<std::string, std::string>> choices;  // value, label
  const char* tip = nullptr;
};

const std::vector<Setting> kSettings = {
    {"Display", "fullscreen", "Fullscreen", Kind::Bool, Toml::Bool, "true", {}},
    {"Display", "window_size", "Window size (windowed)", Kind::Size, Toml::Int, "0x0",
     {{"0x0", "Default"}, {"1280x720", "1280 x 720"}, {"1600x900", "1600 x 900"},
      {"1920x1080", "1920 x 1080"}, {"2560x1440", "2560 x 1440"}},
     "Startup window size when not fullscreen."},
    {"Display", "monitor", "Monitor", Kind::Choice, Toml::Int, "0",
     {{"0", "Default"}, {"1", "1 (primary)"}, {"2", "2"}, {"3", "3"}, {"4", "4"}}},
    {"Display", "aspect_ratio", "Aspect ratio", Kind::Edit, Toml::String, "16:9",
     {{"16:9", "16:9"}, {"21:9", "21:9"}, {"32:9", "32:9"}, {"auto", "auto"}},
     "Wider than 16:9 widens the in-game camera (ultrawide).\n'auto' matches the desktop; you "
     "can also type W:H or a resolution such as 3440x1440."},
    {"Display", "fps_cap", "Frame rate cap", Kind::Edit, Toml::String, "60",
     {{"60", "60"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"},
      {"240", "240"}, {"unlocked", "unlocked"}},
     "60 is stock. Any value from 24 to 240, or 'unlocked'."},
    {"Display", "vsync", "VSync", Kind::Bool, Toml::Bool, "true", {},
     "Off gives lower input latency and smoother pacing here; may tear without a variable "
     "refresh rate display."},

    {"Graphics", "render_quality", "Internal resolution", Kind::Choice, Toml::String, "",
     {{"", "Native (720p)"}, {"2x", "2x (1440p)"}, {"3x", "3x (2160p)"}}},
    {"Graphics", "anisotropic_override", "Anisotropic filtering", Kind::Choice, Toml::Int, "3",
     {{"-1", "Game default"}, {"0", "Off"}, {"1", "1x"}, {"2", "2x"}, {"3", "4x"}, {"4", "8x"},
      {"5", "16x"}}},
    {"Graphics", "native_2x_msaa", "Native 2x MSAA", Kind::Bool, Toml::Bool, "true", {}},

    {"Game", "skip_intro", "Skip intro movie", Kind::Bool, Toml::Bool, "true", {}},
    {"Game", "audio_mute", "Mute audio", Kind::Bool, Toml::Bool, "false", {}},
    {"Game", "mnk_mode", "Keyboard as controller", Kind::Bool, Toml::Bool, "false", {}},

    {"Online", "online", "Online play", Kind::Bool, Toml::Bool, "false", {}},
    {"Online", "online_gamertag", "Gamertag", Kind::Text, Toml::String, "", {},
     "Up to 15 characters; empty uses the profile name."},
    {"Online", "online_server", "Server", Kind::Text, Toml::String, "nhl3on3-turtle.fly.dev", {},
     "Host name or IP address of the NHL 3 on 3 server."},
    {"Online", "online_tunnel", "Connect through server", Kind::Bool, Toml::Bool, "true", {},
     "Sends game traffic through the server so no ports need forwarding.\nTurn off only to "
     "connect to players directly (LAN or VPN)."},
    {"Online", "online_ip", "Advertised IP", Kind::Text, Toml::String, "", {},
     "Only used when not connecting through the server.\nEmpty detects the LAN address; set a "
     "VPN address (Radmin, ZeroTier) to play over it."},
};

constexpr const char* kMarkerLine = "# Set by the NHL3on3 launcher";

fs::path g_dir, g_exe, g_config, g_args_path;
std::map<std::string, std::string> g_values;  // setting key -> value
std::string g_args;

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// "key = value" -> key, value (quotes removed and unescaped).
bool ParseLine(const std::string& line, std::string& key, std::string& value) {
  const size_t eq = line.find('=');
  if (eq == std::string::npos) return false;
  key = Trim(line.substr(0, eq));
  if (key.empty()) return false;
  for (const char c : key) {
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  value = Trim(line.substr(eq + 1));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    std::string out;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
      if (value[i] == '\\' && i + 2 < value.size()) ++i;
      out += value[i];
    }
    value = out;
  }
  return true;
}

std::vector<std::string> ReadLines(const fs::path& path) {
  std::vector<std::string> lines;
  std::ifstream f(path);
  for (std::string line; std::getline(f, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

std::string FormatValue(const std::string& value, Toml toml) {
  if (toml != Toml::String) return value;
  std::string out = "\"";
  for (const char c : value) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

void Load() {
  std::map<std::string, std::string> file;
  for (const std::string& line : ReadLines(g_config)) {
    std::string key, value;
    if (ParseLine(line, key, value)) file[key] = value;
  }
  for (const Setting& s : kSettings) {
    if (s.kind == Kind::Size) {
      const std::string w = file.count("window_width") ? file["window_width"] : "0";
      const std::string h = file.count("window_height") ? file["window_height"] : "0";
      g_values[s.key] = w + "x" + h;
    } else {
      g_values[s.key] = file.count(s.key) ? file[s.key] : s.default_value;
    }
  }
  std::ifstream f(g_args_path);
  std::stringstream ss;
  ss << f.rdbuf();
  g_args = Trim(ss.str());
}

bool Save(std::string& error) {
  // team_play was a launcher setting; the server now sizes team play games itself.
  std::set<std::string> managed = {"team_play"};
  for (const Setting& s : kSettings) {
    if (s.kind == Kind::Size) {
      managed.insert("window_width");
      managed.insert("window_height");
    } else {
      managed.insert(s.key);
    }
  }
  std::vector<std::string> lines;
  for (const std::string& line : ReadLines(g_config)) {
    std::string key, value;
    if ((ParseLine(line, key, value) && managed.count(key)) || line == kMarkerLine) continue;
    lines.push_back(line);
  }
  while (!lines.empty() && Trim(lines.back()).empty()) lines.pop_back();
  std::vector<std::string> ours;
  for (const Setting& s : kSettings) {
    const std::string& value = g_values[s.key];
    if (value == s.default_value) continue;
    if (s.kind == Kind::Size) {
      const size_t x = value.find('x');
      ours.push_back("window_width = " + value.substr(0, x));
      ours.push_back("window_height = " + value.substr(x + 1));
    } else {
      ours.push_back(std::string(s.key) + " = " + FormatValue(value, s.toml));
    }
  }
  if (!ours.empty()) {
    if (!lines.empty()) lines.push_back("");
    lines.push_back(kMarkerLine);
    lines.insert(lines.end(), ours.begin(), ours.end());
  }
  {
    std::ofstream f(g_config, std::ios::trunc);
    for (const std::string& line : lines) f << line << '\n';
    if (!f) {
      error = "Couldn't write " + g_config.string();
      return false;
    }
  }
  std::error_code ec;
  if (!Trim(g_args).empty()) {
    std::ofstream f(g_args_path, std::ios::trunc);
    f << Trim(g_args);
  } else {
    fs::remove(g_args_path, ec);
  }
  return true;
}

// Splits the extra arguments on spaces; "double quotes" group words.
std::vector<std::string> SplitArgs(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, any = false;
  for (const char c : text) {
    if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if (!quoted && (c == ' ' || c == '\t')) {
      if (any) out.push_back(cur);
      cur.clear();
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(cur);
  return out;
}

bool Play(std::string& error) {
  if (access(g_exe.c_str(), X_OK) != 0) {
    error = "nhl3on3 not found (or not executable) in " + g_dir.string();
    return false;
  }
  if (!Save(error)) return false;
  std::vector<std::string> args = SplitArgs(Trim(g_args));
  args.insert(args.begin(), g_exe.string());
  const pid_t pid = fork();
  if (pid < 0) {
    error = "Couldn't start the game";
    return false;
  }
  if (pid == 0) {
    setsid();
    if (chdir(g_dir.c_str()) != 0) _exit(127);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  return true;
}

const std::string* ChoiceLabel(const Setting& s, const std::string& value) {
  for (const auto& [v, label] : s.choices) {
    if (v == value) return &label;
  }
  return nullptr;
}

void Tip(const Setting& s) {
  if (s.tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", s.tip);
}

// An InputText bound to a std::string.
bool InputString(const char* id, std::string& value, float width) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
  ImGui::SetNextItemWidth(width);
  if (ImGui::InputText(id, buffer, sizeof(buffer))) {
    value = buffer;
    return true;
  }
  return false;
}

void SettingRow(const Setting& s, float column, float width) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(s.label);
  Tip(s);
  ImGui::SameLine(column);
  ImGui::PushID(s.key);
  std::string& value = g_values[s.key];
  switch (s.kind) {
    case Kind::Bool: {
      bool on = value == "true";
      if (ImGui::Checkbox("##v", &on)) value = on ? "true" : "false";
      break;
    }
    case Kind::Text:
      InputString("##v", value, width);
      break;
    case Kind::Edit: {
      const float arrow = ImGui::GetFrameHeight();
      InputString("##v", value, width - arrow - ImGui::GetStyle().ItemInnerSpacing.x);
      Tip(s);
      ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
      if (ImGui::ArrowButton("##open", ImGuiDir_Down)) ImGui::OpenPopup("choices");
      if (ImGui::BeginPopup("choices")) {
        for (const auto& [v, label] : s.choices) {
          if (ImGui::Selectable(label.c_str(), v == value)) value = v;
        }
        ImGui::EndPopup();
      }
      break;
    }
    case Kind::Choice:
    case Kind::Size: {
      const std::string* current = ChoiceLabel(s, value);
      if (!current) {
        value = s.default_value;
        current = ChoiceLabel(s, value);
      }
      ImGui::SetNextItemWidth(width);
      if (ImGui::BeginCombo("##v", current ? current->c_str() : value.c_str())) {
        for (const auto& [v, label] : s.choices) {
          if (ImGui::Selectable(label.c_str(), v == value)) value = v;
        }
        ImGui::EndCombo();
      }
      break;
    }
  }
  Tip(s);
  ImGui::PopID();
}

// Draws the window; returns false once the launcher should close.
bool Frame(SDL_Window* window) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::Begin("launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float column = 185 * scale, width = 240 * scale;
  const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y * 3;
  bool keep_open = true;

  ImGui::BeginChild("tabs", ImVec2(0, -footer));
  if (ImGui::BeginTabBar("tabs")) {
    std::vector<std::string> tabs;
    for (const Setting& s : kSettings) {
      if (tabs.empty() || tabs.back() != s.tab) tabs.push_back(s.tab);
    }
    for (const std::string& tab : tabs) {
      if (ImGui::BeginTabItem(tab.c_str())) {
        ImGui::Spacing();
        for (const Setting& s : kSettings) {
          if (tab == s.tab) SettingRow(s, column, width);
        }
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();

  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Extra arguments");
  ImGui::SameLine(column + ImGui::GetStyle().WindowPadding.x);
  InputString("##args", g_args, -FLT_MIN);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Passed to nhl3on3 as-is, for example --log_fps=true --latency_probe=true");
  }

  const ImVec2 button(80 * scale, 0);
  if (ImGui::Button("Defaults", button)) {
    for (const Setting& s : kSettings) g_values[s.key] = s.default_value;
    g_args.clear();
  }
  const float right = ImGui::GetContentRegionMax().x;
  ImGui::SameLine(right - button.x * 2 - 10 * scale - ImGui::GetStyle().ItemSpacing.x);
  std::string error;
  if (ImGui::Button("Save", button) && !Save(error)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NHL 3 on 3 Arcade", error.c_str(), window);
  }
  ImGui::SameLine();
  if (ImGui::Button("Play", ImVec2(button.x + 10 * scale, 0)) ||
      ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    if (Play(error)) {
      keep_open = false;
    } else {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NHL 3 on 3 Arcade", error.c_str(), window);
    }
  }
  ImGui::End();
  return keep_open;
}

void LoadFont(float size) {
  ImGuiIO& io = ImGui::GetIO();
  for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"}) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec) && io.Fonts->AddFontFromFileTTF(path, size)) return;
  }
  io.Fonts->AddFontDefault();
}

}  // namespace

int main(int, char**) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  const char* base = SDL_GetBasePath();
  g_dir = base ? fs::path(base) : fs::current_path();
  g_exe = g_dir / "nhl3on3";
  g_config = g_dir / "nhl3on3.toml";
  g_args_path = g_dir / "nhl3on3_launcher_args.txt";
  Load();

  float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (scale <= 0) scale = 1;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
  style.FrameRounding = 3 * scale;
  style.WindowPadding = ImVec2(12 * scale, 10 * scale);
  style.ItemSpacing = ImVec2(8 * scale, 8 * scale);
  LoadFont(16);

  std::string error;
  if (!launcher_window::Open("NHL 3 on 3 Arcade", int(490 * scale), int(400 * scale), error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    error = "The launcher couldn't start: " + error +
            "\n\nYou can still edit nhl3on3.toml by hand and run nhl3on3 directly.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NHL 3 on 3 Arcade", error.c_str(), nullptr);
    launcher_window::Close();
    ImGui::DestroyContext();
    SDL_Quit();
    return 1;
  }
  SDL_Window* window = launcher_window::Get();

  bool running = true;
  while (running) {
    SDL_Event event;
    SDL_WaitEventTimeout(nullptr, 100);
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        running = false;
      }
    }
    launcher_window::BeginFrame();
    running = Frame(window) && running;
    launcher_window::EndFrame();
  }

  launcher_window::Close();
  ImGui::DestroyContext();
  SDL_Quit();
  return 0;
}
