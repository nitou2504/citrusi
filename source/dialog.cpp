#include <3ds.h>
#include <citro2d.h>
#include <vector>
#include <string>
#include <sstream>
#include "dialog.hpp"
extern "C" {
#include "graphics.h"
}
#include "settings.hpp"

void Dialog::wrapText(std::string text) {
    C2D_TextBuf buf = C2D_TextBufNew(4096);
    float maxWidth = this->width - (MENU_BORDER_HEIGHT * 2) - 20;
    float scale = getFontScale(0.67);

    std::string currentLine = "";
    std::string word = "";
    std::stringstream ss(text);

    while (ss >> word) {
        std::string testLine = currentLine + (currentLine.empty() ? "" : " ") + word;
        C2D_Text ctext;
        C2D_Font font = getFont();
        if (font) {
            C2D_TextFontParse(&ctext, font, buf, testLine.c_str());
        } else {
            C2D_TextParse(&ctext, buf, testLine.c_str());
        }
        float width = 0;
        C2D_TextGetDimensions(&ctext, scale, scale, &width, NULL);

        if (width > maxWidth && !currentLine.empty()) {
            this->message.push_back(currentLine);
            currentLine = word;
        } else {
            currentLine = testLine;
        }
    }
    if (!currentLine.empty()) {
        this->message.push_back(currentLine);
    }
    C2D_TextBufDelete(buf);
}

// A Dialog renders with the SAME visual language as actionMenu (menu.cpp):
// full-screen, centered title + subtitle from the message lines, then
// full-width left-aligned accent option rows, and a bottom hint. This is the
// one surface every confirm / OK / error uses, matching the pickers.
void Dialog::draw() {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(this->target, COL_BG);
    C2D_SceneBegin(this->target);
    float cx = this->x + this->width/2;
    size_t n = this->options.size();
    size_t M = this->message.size();
    // message[0] = title; message[1..] = subtitle lines (as in actionMenu)
    if (M > 0)
        drawText(cx, this->y + 16, 0.5f, 0.55f, 0, COL_TEXT, this->message[0].c_str(), C2D_AlignCenter);
    float sy = this->y + 34;
    for (size_t i = 1; i < M; i++, sy += 14.0f)
        drawText(cx, sy, 0.5f, 0.42f, 0, COL_TEXT_DIM, this->message[i].c_str(), C2D_AlignCenter);
    // option rows: identical geometry/colours to actionMenu (rect x+12 w-24 h22,
    // label x+22, vertically centred, accent when selected)
    float pitch = 24.0f, rowH = 22.0f;
    float hintY = this->y + this->height - 10.0f;
    float oy = (M > 1) ? sy + 8.0f : this->y + 50.0f;   // below subtitle, else actionMenu default
    float maxOy = hintY - 12.0f - (float)n*pitch;       // never overlap the hint
    if (oy > maxOy) oy = maxOy;
    if (oy < this->y + 30.0f) oy = this->y + 30.0f;
    for (size_t i = 0; i < n; i++) {
        float ry = oy + (float)i*pitch;
        bool hot = (this->selected == i);
        if (hot) C2D_DrawRectSolid(this->x+12, ry, 0.4f, this->width-24, rowH, COL_ACCENT);
        drawText(this->x+22, ry + rowH*0.5f, 0.5f, 0.5f, 0,
                 hot ? HIGHLIGHT_FOREGROUND : COL_TEXT_DIM, this->options[i].c_str(), 0);
    }
    if (n > 0)
        drawText(cx, hintY, 0.5f, 0.4f, 0, COL_TEXT_DIM,
                 (n > 1 ? "A select    B back" : "A OK"), C2D_AlignCenter);
    C3D_FrameEnd(0);
}
int Dialog::handle() {
    touchPosition pos;
    touchPosition oldPos;
    if (this->options.size() < 1) {
        this->draw();
        return -1;
    }
    while (aptMainLoop())
    {
        hidScanInput();
        oldPos = pos;
		u32 kDown = hidKeysDown();
        hidTouchRead(&pos);
        
        if (pos.px != oldPos.px || pos.py != oldPos.py) {
            // check tap position
        }
        
        // vertical list: UP/DOWN primary, LEFT/RIGHT kept as aliases
        if (kDown & (KEY_UP|KEY_LEFT)) {
            if (this->selected > 0) {
                this->selected--;
            }
            else if(this->options.size() > 0) {
                this->selected=this->options.size()-1;
            }
        }else if(kDown & (KEY_DOWN|KEY_RIGHT)) {
            this->selected++;
            if (this->selected >= this->options.size()) this->selected=0;
        }else if((kDown & KEY_A) || (kDown & KEY_START)) {
            return this->selected;
        }else if(kDown & KEY_B) {
            return -1; // cancel
        }
        this->draw();
    }
    return -1;
}
// full-frame loading screen. Renders through the real render targets (both
// screens) and leaves the message on-screen while the caller's blocking work
// runs afterwards — the framebuffer keeps showing it until the next frame.
static C3D_RenderTarget* gLoadTop = nullptr;
static C3D_RenderTarget* gLoadBottom = nullptr;
void gLoadingTargets(C3D_RenderTarget* top, C3D_RenderTarget* bottom) {
    gLoadTop = top;
    gLoadBottom = bottom;
}

void showLoading(C3D_RenderTarget* target, std::initializer_list<std::string> message) {
    (void)target;
    if (!gLoadBottom) return;
    std::vector<std::string> lines(message.begin(), message.end());
    // draw a couple of frames so the present definitely lands before blocking
    for (int f = 0; f < 2; f++) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        if (gLoadTop) { C2D_TargetClear(gLoadTop, BGColor); }
        C2D_TargetClear(gLoadBottom, BGColor);
        C2D_SceneBegin(gLoadBottom);
        float y = 240.0f / 2 - (lines.size() * 9);
        for (auto& l : lines) {
            drawText(160, y, 0.5f, 0.5f, 0, FOREGROUND_COLOR, l.c_str(), C2D_AlignCenter);
            y += 18;
        }
        C3D_FrameEnd(0);
    }
}

Dialog::Dialog(C3D_RenderTarget* target, float x, float y, float width, float height, std::string message, std::initializer_list<std::string> options, int defaultChoice) {
    this->options = std::vector(options.begin(),options.end());
    this->target=target;
    this->selected=defaultChoice;
    this->x=x;
    this->y=y;
    this->width=width;
    this->height=height;
    this->wrapText(message);
}
Dialog::Dialog(C3D_RenderTarget* target, float x, float y, float width, float height, std::initializer_list<std::string> message, std::initializer_list<std::string> options, int defaultChoice) {
    this->options = std::vector(options.begin(),options.end());
    this->target=target;
    this->selected=defaultChoice;
    this->x=x;
    this->y=y;
    this->width=width;
    this->height=height;
    for (auto m : message) this->wrapText(m);
}
Dialog::Dialog(C3D_RenderTarget* target, float x, float y, float width, float height, const std::vector<std::string>& message, std::initializer_list<std::string> options, int defaultChoice) {
    this->options = std::vector(options.begin(),options.end());
    this->target=target;
    this->selected=defaultChoice;
    this->x=x;
    this->y=y;
    this->width=width;
    this->height=height;
    for (auto& m : message) this->wrapText(m);
}

