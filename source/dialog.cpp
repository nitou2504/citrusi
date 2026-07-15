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

// measure a string's width at a given (already-scaled) font size
static float dlgTextW(const std::string& s, float fscale) {
    C2D_TextBuf b = C2D_TextBufNew(1024);
    C2D_Text t; C2D_Font f = getFont();
    if (f) C2D_TextFontParse(&t, f, b, s.c_str()); else C2D_TextParse(&t, b, s.c_str());
    float w = 0; C2D_TextGetDimensions(&t, fscale, fscale, &w, NULL);
    C2D_TextBufDelete(b);
    return w;
}
// filled rounded rectangle (pill) — rects + corner circles
static void dlgPill(float x, float y, float z, float w, float h, float r, u32 c) {
    if (r*2 > h) r = h/2;
    if (r*2 > w) r = w/2;
    C2D_DrawRectSolid(x+r, y, z, w-2*r, h, c);
    C2D_DrawRectSolid(x, y+r, z, w, h-2*r, c);
    C2D_DrawCircleSolid(x+r,   y+r,   z, r, c);
    C2D_DrawCircleSolid(x+w-r, y+r,   z, r, c);
    C2D_DrawCircleSolid(x+r,   y+h-r, z, r, c);
    C2D_DrawCircleSolid(x+w-r, y+h-r, z, r, c);
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
    // A confirm modal: centered message (the prompt), then compact, content-
    // sized pill buttons centered below it — not full-width list bars (a lone
    // OK as a screen-wide bar reads as heavy/unbalanced). Selected pill = accent
    // fill + light label; the rest = elevated fill + dim label. No separate hint
    // row: the highlighted pill is the affordance (A confirms, B cancels).
    size_t n = this->options.size();
    size_t M = this->message.size();
    float lineH = 1.0f + textheight;
    float gap   = M ? 14.0f : 0.0f;         // message -> buttons
    float bh = 30.0f, bgap = 10.0f;         // pill height, gap between pills
    float bscale = 0.5f, bfs = getFontScale(bscale);
    // uniform pill width = widest label + padding, clamped to the card
    float bw = 84.0f;
    for (auto& o : this->options) { float w = dlgTextW(o, bfs) + 40.0f; if (w > bw) bw = w; }
    if (bw > this->width - 36.0f) bw = this->width - 36.0f;
    float areaT = this->y + MENU_BORDER_HEIGHT + 10.0f;
    float areaB = this->y + this->height - MENU_BORDER_HEIGHT - 10.0f;
    // clamp message lines so the group always fits
    float btnBlock = (float)n*bh + (n>0?(float)(n-1)*bgap:0.0f);
    float roomForMsg = areaB - areaT - btnBlock - gap;
    size_t maxMsg = (lineH > 0 && roomForMsg > 0) ? (size_t)(roomForMsg / lineH) : 0;
    if (M > maxMsg) M = maxMsg;
    float blockH = (float)M*lineH + gap + btnBlock;
    float top = areaT + (areaB - areaT - blockH) * 0.5f;
    if (top < areaT) top = areaT;
    // message (prompt): centered lines
    float my = top + lineH*0.5f;
    for (size_t i=0;i<M;i++, my += lineH)
        drawText(cx, my, 0, 0.62f, 0, FOREGROUND_COLOR, this->message[i].c_str(), C2D_AlignCenter);
    // pill buttons, centered and stacked
    float oy = top + (float)M*lineH + gap;
    float bx = cx - bw*0.5f;
    for (size_t i=0;i<n;i++) {
        float by = oy + (float)i*(bh+bgap);
        bool hot = (this->selected==(int)i);
        dlgPill(bx, by, 0.4f, bw, bh, 8.0f, hot?COL_ACCENT:COL_ELEV);
        drawText(cx, by + bh*0.5f, 0.5f, bscale, 0,
                 hot?HIGHLIGHT_FOREGROUND:COL_TEXT_DIM, this->options[i].c_str(), C2D_AlignCenter);
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

