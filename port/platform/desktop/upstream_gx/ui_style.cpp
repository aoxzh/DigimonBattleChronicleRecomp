#include "ui_style.h"
#include "imgui.h"
#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

float ui_display_scale(void) {
    float ddpi = 96.0f;
    if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) != 0 || ddpi <= 0) ddpi = 96.0f;
#if defined(__ANDROID__)
    // Android reports the panel's physical density (a large phone panel: ~375-400 dpi); 160 dpi = 1 dp.
    return std::clamp(ddpi / 160.0f * 0.95f, 1.0f, 4.0f);
#elif defined(__APPLE__)
    return 1.0f;   // macOS reports points; the drawable is already Retina
#else
    return std::clamp(ddpi / 96.0f, 1.0f, 3.0f);
#endif
}

void ui_apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 8; st.FrameRounding = 5; st.GrabRounding = 5; st.TabRounding = 5;
    st.FramePadding = ImVec2(9, 5); st.ItemSpacing = ImVec2(9, 7); st.WindowPadding = ImVec2(18, 14);
    st.FrameBorderSize = 1.0f;
    st.ScrollbarSize = 16;
    ImVec4 *c = st.Colors;
    const ImVec4 accent(0.93f, 0.50f, 0.13f, 1.0f), accent_hi(1.0f, 0.62f, 0.25f, 1.0f), accent_lo(0.62f, 0.32f, 0.08f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.085f, 0.11f, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0.095f, 0.105f, 0.135f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.21f, 0.26f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.27f, 0.33f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.31f, 0.38f, 1);
    c[ImGuiCol_Border] = ImVec4(0.30f, 0.31f, 0.37f, 1);
    c[ImGuiCol_Button] = ImVec4(0.19f, 0.20f, 0.25f, 1);
    c[ImGuiCol_ButtonHovered] = accent_lo; c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.19f, 0.20f, 0.25f, 1); c[ImGuiCol_HeaderHovered] = accent_lo; c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.18f, 1); c[ImGuiCol_TabHovered] = accent_lo; c[ImGuiCol_TabSelected] = accent_lo;
    c[ImGuiCol_CheckMark] = accent_hi; c[ImGuiCol_SliderGrab] = accent; c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_Separator] = ImVec4(0.25f, 0.26f, 0.31f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_NavCursor] = accent_hi;
    st.ScaleAllSizes(scale);
}

static bool exists(const std::string &path) {
    if (FILE *f = fopen(path.c_str(), "rb")) { fclose(f); return true; }
    return false;
}

void ui_load_fonts(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    const float size = 17.0f * scale;
    std::string base, cjk;
#if defined(_WIN32)
    const char *windir = getenv("WINDIR");
    std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    base = fonts + "segoeui.ttf";
    for (const char *name : {"msyh.ttc", "YuGothM.ttc", "meiryo.ttc"})
        if (exists(fonts + name)) { cjk = fonts + name; break; }
#elif defined(__ANDROID__)
    // Roboto-Regular.ttf is a variable font on recent Android; the static cut loads reliably.
    for (const char *name : {"/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf"})
        if (exists(name)) { base = name; break; }
    for (const char *name : {"/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansSC-Regular.otf",
                             "/system/fonts/SECCJK-Regular.ttc"})
        if (exists(name)) { cjk = name; break; }
#else
    for (const char *name : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"})
        if (exists(name)) { base = name; break; }
#endif
    // Build here so an unusable font file (e.g. a CFF-outline .ttc stb_truetype rejects)
    // falls back instead of leaving the atlas empty: base+CJK, then base, then built-in.
    for (int attempt = 0; attempt < 3; ++attempt) {
        io.Fonts->Clear();
        ImFont *font = nullptr;
        if (attempt < 2 && !base.empty() && exists(base)) {
            ImFontConfig cfg; cfg.OversampleH = 2;
            font = io.Fonts->AddFontFromFileTTF(base.c_str(), size, &cfg);
            if (font && attempt == 0 && !cjk.empty()) {
                ImFontConfig merge; merge.MergeMode = true; merge.OversampleH = 1;
                io.Fonts->AddFontFromFileTTF(cjk.c_str(), size, &merge, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            }
        }
        if (!font) { ImFontConfig d; d.SizePixels = 13.0f * scale; io.Fonts->AddFontDefault(&d); }
        if (io.Fonts->Build()) return;
        fprintf(stderr, "[ui] font atlas failed (attempt %d: %s%s); falling back\n", attempt, base.c_str(),
                attempt == 0 && !cjk.empty() ? " + CJK" : "");
    }
}
