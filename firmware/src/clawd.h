// 画面に Claude のマスコット (Clawd) を出し、動いていることが分かるようにする。
// 起きているときは LottieFiles のアニメーション (clawd_frames.h) を数秒おきに再生する。
// 夜間停止中や暗いときは、同じ姿のドット絵で目を閉じて眠る (z が出る)。
#pragma once
#include <M5GFX.h>
#include "clawd_frames.h"

namespace clawd {

// 眠っている姿。23 x 12 マス (足は別に描く)。o=体、s=体の影、e=目 (閉じて横線)、.=なし
static const int GRID_W = 23;
static const char* const BODY[12] = {
    "...ooooooooooooosss....",
    "...ooooooooooooosss....",
    "...ooooooooooooosss....",
    "...oeeooooooeeoosss....",
    "ooooooooooooooooooossss",
    "ooooooooooooooooooossss",
    "ooooooooooooooooooossss",
    "ooooooooooooooooooossss",
    "...ooooooooooooosss....",
    "...ooooooooooooosss....",
    "...ooooooooooooosss....",
    "...ooooooooooooosss....",
};
static const int LEG_COLS[4] = {4, 8, 12, 16};  // 足の左端の列 (幅2マス、体の下に3マス)
static const int CELL = 8;                      // 1マスの画素数 (アニメーションと同じくらいの大きさにする)

static const int CANVAS_W = ANIM_W;
static const int CANVAS_H = ANIM_H;
static const uint32_t ANIM_FRAME_MS = 80;     // 元は 25fps。描画の負担を抑えるため少し遅くする
static const uint32_t ANIM_PAUSE_MS = 4000;   // 1回再生したら最初のコマで止まる時間
static const uint32_t SLEEP_FRAME_MS = 1200;  // 眠っているときに z を変える間隔

static const uint16_t COLOR_BODY = lgfx::color565(212, 116, 84);
static const uint16_t COLOR_SHADE = lgfx::color565(186, 101, 74);
static const uint16_t COLOR_EYE = lgfx::color565(17, 17, 17);
static const uint16_t COLOR_ZZZ = lgfx::color565(120, 120, 120);

class Animator {
public:
  void begin(LGFX_Device* display) {
    _display = display;
    _canvas = new M5Canvas(display);
    _canvas->setColorDepth(16);
    _canvas->setPsram(true);
    _canvas->createSprite(CANVAS_W, CANVAS_H);
  }

  // loop から何度呼んでもよい。コマを進める時刻になったときだけ描き直す
  void update(uint32_t now, bool sleeping) {
    if (!_canvas) return;
    if (sleeping != _sleeping) {
      _sleeping = sleeping;
      _frame = 0;
      _nextMs = now;
    }
    if ((int32_t)(now - _nextMs) < 0) return;
    if (_sleeping) {
      drawSleeping(_frame++);
      _nextMs = now + SLEEP_FRAME_MS;
    } else {
      drawAnimFrame(_frame);
      // 最後のコマまで再生したら、最初のコマに戻ってしばらく止まる
      _frame = (_frame + 1) % ANIM_FRAMES;
      _nextMs = now + (_frame == 0 ? ANIM_PAUSE_MS : ANIM_FRAME_MS);
    }
    // 一番上の行は showStatus の文字に使うので、少し下にずらす
    _canvas->pushSprite((_display->width() - CANVAS_W) / 2, (_display->height() - CANVAS_H) / 2 + 12);
  }

private:
  void drawAnimFrame(int f) {
    const uint8_t* runs = ANIM_FRAME_DATA[f];
    int len = ANIM_FRAME_LEN[f];
    int x = 0, y = 0;
    for (int i = 0; i + 1 < len; i += 2) {
      int n = runs[i];
      _canvas->drawFastHLine(x, y, n, ANIM_PALETTE[runs[i + 1]]);
      x += n;
      if (x >= ANIM_W) {
        x = 0;
        y++;
      }
    }
  }

  void drawSleeping(uint32_t f) {
    _canvas->fillSprite(TFT_BLACK);
    int ox = (CANVAS_W - GRID_W * CELL) / 2;
    int oy = CANVAS_H - 15 * CELL;  // 足が下端に来るように置く
    auto cell = [&](int c, int r, uint16_t color) { _canvas->fillRect(ox + c * CELL, oy + r * CELL, CELL, CELL, color); };
    for (int r = 0; r < 12; r++) {
      for (int c = 0; c < GRID_W; c++) {
        char ch = BODY[r][c];
        if (ch == '.') continue;
        cell(c, r, ch == 's' ? COLOR_SHADE : ch == 'e' ? COLOR_EYE : COLOR_BODY);
      }
    }
    for (int i = 0; i < 4; i++) {
      uint16_t color = i == 3 ? COLOR_SHADE : COLOR_BODY;
      for (int r = 12; r < 15; r++) {
        cell(LEG_COLS[i], r, color);
        cell(LEG_COLS[i] + 1, r, color);
      }
    }
    _canvas->setTextColor(COLOR_ZZZ);
    _canvas->setTextSize(2);
    _canvas->setCursor(CANVAS_W - 40, 0);
    _canvas->print(f % 3 == 0 ? "z" : f % 3 == 1 ? "zz" : "zzz");
  }

  LGFX_Device* _display = nullptr;
  M5Canvas* _canvas = nullptr;
  uint32_t _nextMs = 0;
  uint32_t _frame = 0;
  bool _sleeping = false;
};

}  // namespace clawd
