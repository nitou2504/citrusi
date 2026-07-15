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

void Dialog::draw() {
    C2D_TextBuf buf = C2D_TextBufNew(4096);
    C2D_Text ctext;
    C2D_TextParse(&ctext,buf,"0");
    float textheight=0;
    float scale = getFontScale(0.67);
    C2D_TextGetDimensions(&ctext,scale,scale,NULL,&textheight);
    C2D_TextBufDelete(buf);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(this->target, BGColor); // avoid stale double-buffer bleed
    C2D_SceneBegin(this->target);
    drawPanel(this->x,this->y,0,this->width, this->height,MENU_BORDER_HEIGHT,BGColor,BORDER_COLOR);
    float cx = this->x + this->width/2;
    // options stack VERTICALLY, bottom-anchored, accent highlight — same look
    // as the actionMenu vertical picker (horizontal button rows read as cramped)
    size_t n = this->options.size();
    float pitch = 26.0f, rowH = 22.0f;
    float oy = this->y + this->height - MENU_BORDER_HEIGHT - 8 - (float)n*pitch;
    // message band: top padding down to just above the options. Clamp the
    // number of lines so a tall wrapped message can never paint under the
    // accent bar ("text behind a color bar"). Shrink line pitch when crowded.
    float lineH  = 1.0f + textheight;
    float msgTop = this->y + this->height*2.0f/12.0f;
    float msgBot = oy - 6.0f;                       // 6px gap above the first option
    float band   = msgBot - msgTop;
    size_t M = this->message.size();
    if (M > 0 && lineH * (float)M > band)           // squeeze to fit the band
        lineH = band / (float)M;
    size_t maxMsg = (band > 0) ? (size_t)(band / lineH) : 1;
    if (maxMsg < 1) maxMsg = 1;
    for (size_t i=0;i<M && i<maxMsg;i++)
        drawText(cx, msgTop + lineH*(float)i, 0, 0.67, 0, FOREGROUND_COLOR,
                 this->message[i].c_str(), C2D_AlignCenter);
    for (size_t i=0;i<n;i++) {
        float ry = oy + (float)i*pitch;
        bool hot = (this->selected==(int)i);
        if (hot) C2D_DrawRectSolid(this->x+12, ry, 0.4f, this->width-24, rowH, HIGHLIGHT_BGCOLOR);
        drawText(cx, ry+rowH/2 - 6, 0, 0.6f, 0,
                 hot?HIGHLIGHT_FOREGROUND:FOREGROUND_COLOR, this->options[i].c_str(), C2D_AlignCenter);
    }
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

