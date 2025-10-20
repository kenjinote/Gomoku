// Gomoku (五目並べ) - Win32API + Direct2D/DirectWrite, Human vs AI, with animations, strong AI, and threaded thinking UI
// Build with: /DUNICODE /D_UNICODE and link d2d1.lib, windowscodecs.lib, dwrite.lib

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <wincodec.h>
#include <dwrite.h> // DirectWriteを追加
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <chrono>
#include <unordered_map>
#include <array>
#include <random>
#include <thread>
#include <atomic>
#include "resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib") // DirectWriteライブラリのリンクを追加

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) if(p){ (p)->Release(); (p)=nullptr; }
#endif

// -------------------- Config --------------------
static const int BOARD_SIZE = 15;
static const int CELL_SIZE_BASE = 40; // 基準となるセルサイズ
static const int MARGIN_BASE = 40;     // 基準となるマージン
static const int STONE_RADIUS_BASE = 16; // 基準となる石の半径
static const int BOARD_PX_BASE = MARGIN_BASE * 2 + CELL_SIZE_BASE * (BOARD_SIZE - 1); // 盤面全体の基準幅/高さ
static const int WINDOW_W_BASE = BOARD_PX_BASE;
static const int WINDOW_H_BASE = BOARD_PX_BASE + 60; // ターン表示エリア込みの基準高さ
static const int TIMER_FPS = 16; // ~60 FPS
static const float BASE_FONT_SIZE = 24.0f; // 基準となるフォントサイズ

// -------------------- Game State --------------------
enum class Player : int { Empty = 0, Black = 1, White = 2 };
using BoardType = Player[BOARD_SIZE][BOARD_SIZE];
struct Move { int x, y; Player p; };
struct WinLine { bool has = false; int x1 = 0, y1 = 0, x2 = 0, y2 = 0; };

static Player g_board[BOARD_SIZE][BOARD_SIZE];
static Player g_current = Player::Black; // Human=Black, AI=White
static bool g_gameOver = false;
static WinLine g_winLine;
static std::vector<Move> g_history;

// Animation for last placed stone
struct StoneAnim {
    bool active = false; int x = 0, y = 0; Player p = Player::Empty;
    double t0 = 0.0; // start time (seconds)
    double dur = 0.2; // seconds
    bool is_undo = false; // 消滅アニメーションかどうかのフラグ (単一石)
    bool reset_anim = false; // リセットアニメーション全体のフラグ
} g_anim;

// Thinking state (UI + input lock)
static std::atomic<bool> g_aiThinking{ false };
static HWND g_hwndMain = nullptr;
const UINT WM_APP_AIMOVE = WM_APP + 1; // lParam: MAKELPARAM(x,y)

// --- Dynamic Scaling Variables ---
static float g_scale = 1.0f;
static float g_offsetX = 0.0f;
static float g_offsetY = 0.0f;
static float g_windowW = (float)WINDOW_W_BASE; // 現在のウィンドウ幅
static float g_windowH = (float)WINDOW_H_BASE; // 現在のウィンドウ高さ
static float g_current_cell_size = (float)CELL_SIZE_BASE;
// ---------------------------------------------

// DPIスケーリング用の変数
static float g_current_dpi_scale = 1.0f; // 実際のDPI/96.0f
// ---------------------------------------------

// Simple time function
static double NowSec() {
    static auto t0 = std::chrono::high_resolution_clock::now();
    auto t = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> dt = t - t0; return dt.count();
}

// -------------------- Forward Decls --------------------
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void ResetGame();
bool PlaceStone(int x, int y, Player p, bool animate = true);
bool CheckWin(const BoardType board, int x, int y, Player p, WinLine& out);
void OnResize(UINT width, UINT height);
void StartAIThink(); // thread spawner
void ResetGameImmediate(); // 盤面クリアのみを行う関数

// -------------------- Direct2D (and DirectWrite) --------------------
ID2D1Factory* g_pFactory = nullptr;
ID2D1HwndRenderTarget* g_pRT = nullptr;
ID2D1SolidColorBrush* g_pGridBrush = nullptr;
ID2D1SolidColorBrush* g_pBlackBrush = nullptr;
ID2D1SolidColorBrush* g_pWhiteBrush = nullptr;
ID2D1SolidColorBrush* g_pTextBrush = nullptr;
ID2D1SolidColorBrush* g_pWinBrush = nullptr;

// --- DirectWrite Resources ---
IDWriteFactory* g_pDWFactory = nullptr;
IDWriteTextFormat* g_pTextFormat = nullptr;
// -----------------------------

// DirectWriteのテキストフォーマットをDPIに基づいて再生成するヘルパー関数
HRESULT CreateTextFormat(float dpiScale) {
    SAFE_RELEASE(g_pTextFormat); // 既存のフォーマットを解放

    if (!g_pDWFactory) return E_FAIL;

    HRESULT hr = g_pDWFactory->CreateTextFormat(
        L"Meiryo UI",         // フォント名
        NULL,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        BASE_FONT_SIZE * dpiScale, // DPIスケーリングを適用
        L"",                  // ロケール
        &g_pTextFormat);

    if (SUCCEEDED(hr)) {
        g_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return hr;
}


HRESULT CreateGraphicsResources(HWND hwnd) {
    HRESULT hr = S_OK;
    if (!g_pRT) {
        RECT rc; GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        // DirectWrite Factoryの作成
        if (!g_pDWFactory) {
            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_pDWFactory));
        }

        // テキストフォーマットの作成 (DPIスケーリングを適用)
        if (SUCCEEDED(hr) && !g_pTextFormat) {
            hr = CreateTextFormat(g_current_dpi_scale);
        }

        if (SUCCEEDED(hr)) {
            hr = g_pFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(hwnd, size),
                &g_pRT);
        }

        if (SUCCEEDED(hr)) {
            g_pRT->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f), &g_pGridBrush);
            g_pRT->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_pBlackBrush);
            g_pRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &g_pWhiteBrush);
            g_pRT->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.45f, 0.10f), &g_pTextBrush);
            g_pRT->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.2f, 0.2f, 0.8f), &g_pWinBrush);
        }
    }
    return hr;
}

void DiscardGraphicsResources() {
    SAFE_RELEASE(g_pGridBrush);
    SAFE_RELEASE(g_pBlackBrush);
    SAFE_RELEASE(g_pWhiteBrush);
    SAFE_RELEASE(g_pTextBrush);
    SAFE_RELEASE(g_pWinBrush);
    SAFE_RELEASE(g_pRT);
    // DirectWrite Resourcesの解放
    SAFE_RELEASE(g_pTextFormat);
    SAFE_RELEASE(g_pDWFactory);
}

void OnResize(UINT width, UINT height) {
    if (g_pRT) {
        g_pRT->Resize(D2D1::SizeU(width, height));
    }

    g_windowW = (float)width;
    g_windowH = (float)height;

    // 盤面エリア（ターン表示エリアを除いた部分）の基準サイズ
    const float board_w_base = (float)BOARD_PX_BASE;
    const float board_h_base = (float)BOARD_PX_BASE;

    // ウィンドウサイズからターン表示エリアの高さを引いた、盤面描画に使える高さ
    // ターン表示エリアの高さは WINDOW_H_BASE - BOARD_PX_BASE (固定値)
    const float turn_area_h_base = (float)(WINDOW_H_BASE - BOARD_PX_BASE);
    const float effective_h = g_windowH - turn_area_h_base; // スケーリング前の固定高さ分を引く

    // スケーリング係数を計算 (幅と高さのうち、小さい方に合わせる)
    float scale_w = g_windowW / board_w_base;
    float scale_h = effective_h / board_h_base;

    // スケールを決定 (アスペクト比を維持するため、小さい方を選ぶ)
    g_scale = min(scale_w, scale_h);

    // 新しいセルのサイズ
    g_current_cell_size = CELL_SIZE_BASE * g_scale;

    // 盤面全体の描画領域（外側のマージン込み）のピクセルサイズ
    float total_board_pixel_w = ((float)BOARD_SIZE - 1) * g_current_cell_size + (2 * MARGIN_BASE * g_scale);
    float total_board_pixel_h = ((float)BOARD_SIZE - 1) * g_current_cell_size + (2 * MARGIN_BASE * g_scale);

    // 中央寄せのためのオフセットを計算

    // 1. X軸オフセット: ウィンドウ幅の中央に配置
    g_offsetX = (g_windowW - total_board_pixel_w) / 2.0f;

    // 2. Y軸オフセット: effective_h の中で盤面を中央に配置
    g_offsetY = (effective_h - total_board_pixel_h) / 2.0f;

    // 3. 盤面の左上（0,0）の交点が、計算されたオフセット位置から始まるように、内部マージン分を加算
    // g_offsetX, g_offsetY は盤面の左上（0,0）交点のピクセル座標を示す
    g_offsetX += MARGIN_BASE * g_scale;
    g_offsetY += MARGIN_BASE * g_scale;
}

// 盤面座標(x, y)を画面ピクセル座標(px, py)に変換する関数
D2D1_POINT_2F BoardToPx(int x, int y) {
    float px = g_offsetX + x * g_current_cell_size;
    float py = g_offsetY + y * g_current_cell_size;
    return D2D1::Point2F(px, py);
}

// DrawStoneの修正: シンプルにスケールのみを使用
void DrawStone(int x, int y, Player p, float scale_anim = 1.0f) {
    auto c = BoardToPx(x, y);
    // 石の半径もスケールを適用
    float r = STONE_RADIUS_BASE * g_scale * scale_anim;
    ID2D1SolidColorBrush* fill = (p == Player::Black) ? g_pBlackBrush : g_pWhiteBrush;

    // 描画
    g_pRT->FillEllipse(D2D1::Ellipse(c, r, r), fill);
    g_pRT->DrawEllipse(D2D1::Ellipse(c, r, r), g_pGridBrush, 1.5f * g_scale);
}

float EaseOutBack(float t) { float s = 1.70158f; t = (t - 1); return (t * t * ((s + 1) * t + s) + 1); }

void DrawThinkingOverlay() {
    if (!g_aiThinking.load()) return;
    // シンプルな点滅バーを画面下部に表示（ウィンドウ下端に固定）
    double t = NowSec();
    float a = 0.25f + 0.15f * (float)(0.5 * (sin(t * 6.28318 * 0.8) + 1));
    ID2D1SolidColorBrush* brush = nullptr;
    g_pRT->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, a), &brush);

    // バーの描画位置をウィンドウサイズに合わせて調整
    float bar_h = 30.0f;
    D2D1_RECT_F r = D2D1::RectF(
        10.f,
        g_windowH - bar_h - 10.f,
        g_windowW - 10.f,
        g_windowH - 10.f
    );
    g_pRT->FillRectangle(r, brush);
    SAFE_RELEASE(brush);
}

void OnPaint(HWND hwnd) {
    if (FAILED(CreateGraphicsResources(hwnd))) return;
    PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
    g_pRT->BeginDraw();
    g_pRT->Clear(D2D1::ColorF(0.96f, 0.93f, 0.85f));

    // grid
    for (int i = 0; i < BOARD_SIZE; i++) {
        // 水平線
        auto p1 = BoardToPx(0, i);
        auto p2 = BoardToPx(BOARD_SIZE - 1, i);
        g_pRT->DrawLine(p1, p2, g_pGridBrush, 1.0f * g_scale);

        // 垂直線
        p1 = BoardToPx(i, 0);
        p2 = BoardToPx(i, BOARD_SIZE - 1);
        g_pRT->DrawLine(p1, p2, g_pGridBrush, 1.0f * g_scale);
    }

    // star points
    auto drawStar = [&](int x, int y) {
        auto c = BoardToPx(x, y);
        g_pRT->FillEllipse(D2D1::Ellipse(c, 3.0f * g_scale, 3.0f * g_scale), g_pGridBrush); // サイズにスケール適用
        };
    int s = BOARD_SIZE, k = (s - 1) / 2;
    drawStar(3, 3); drawStar(3, k); drawStar(3, s - 4);
    drawStar(k, 3); drawStar(k, k); drawStar(k, s - 4);
    drawStar(s - 4, 3); drawStar(s - 4, k); drawStar(s - 4, s - 4);

    // stones
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            Player p = g_board[y][x];

            bool isAnimTarget = g_anim.active && (g_anim.reset_anim || (g_anim.x == x && g_anim.y == y));

            // 盤面に石がない場合は、リセットアニメーション中であってもスキップする (修正箇所)
            if (p == Player::Empty) {
                // ただし、単一Undoアニメーションの場合、g_boardから削除済みだがg_animに情報が残っているので、
                // その座標がg_anim.x, g_anim.yと一致する場合のみ、pをg_anim.pでオーバーライドして描画を続行させる。
                if (g_anim.active && g_anim.is_undo && g_anim.x == x && g_anim.y == y) {
                    p = g_anim.p;
                }
                else {
                    continue; // 盤面が空で、アニメーションも関係ない場合は描画しない
                }
            }

            // アニメーション処理
            if (isAnimTarget) {
                double t = (NowSec() - g_anim.t0) / g_anim.dur;
                if (t < 0) t = 0;

                float sc = 1.0f;
                Player anim_p = p;

                if (g_anim.is_undo && !g_anim.reset_anim) {
                    // 単一のUndoアニメーション
                    anim_p = g_anim.p;
                }

                if (t < 1.0) {
                    if (g_anim.is_undo || g_anim.reset_anim) {
                        // 消滅アニメーション (Undo / Reset)
                        sc = (float)(1.0 - t); // スケールを1から0に
                        DrawStone(x, y, anim_p, sc);
                    }
                    else {
                        // 設置アニメーション (PlaceStone)
                        sc = EaseOutBack((float)t);
                        DrawStone(x, y, anim_p, sc);
                    }
                }
                else {
                    // アニメーション終了 (WM_TIMERで処理される)
                }
            }
            // 盤面上の通常の石 (アニメーション中ではない場合)
            else if (p != Player::Empty) {
                DrawStone(x, y, p, 1.0f);
            }
        }
    }

    // win line
    if (g_winLine.has) {
        auto a = BoardToPx(g_winLine.x1, g_winLine.y1);
        auto b = BoardToPx(g_winLine.x2, g_winLine.y2);

        D2D1_POINT_2F dir = D2D1::Point2F(b.x - a.x, b.y - a.y);
        float len = max(1.0f, std::sqrt(dir.x * dir.x + dir.y * dir.y));
        dir.x /= len;
        dir.y /= len;

        // 線を少し延長する距離もスケールを適用
        float extend = 10.0f * g_scale;
        a.x -= dir.x * extend; a.y -= dir.y * extend;
        b.x += dir.x * extend; b.y += dir.y * extend;

        g_pRT->DrawLine(a, b, g_pWinBrush, 4.0f * g_scale); // 線幅にスケールを適用
    }

    // --- ターン表示テキストの描画 ---
    if (!g_gameOver && g_pRT && g_pTextFormat) {
        const wchar_t* turnText = L"";
        D2D1_COLOR_F textColor;

        if (g_aiThinking.load()) {
			WCHAR szText[256];
			LoadString(0, IDS_AI_THINKING, szText, _countof(szText));
            turnText = szText;
            textColor = D2D1::ColorF(0.5f, 0.0f, 0.0f);
        }
        else if (g_current == Player::Black) {
            WCHAR szText[256];
			LoadString(0, IDS_YOUR_TURN, szText, _countof(szText));            
            turnText = szText;
            textColor = D2D1::ColorF(0.0f, 0.0f, 0.0f);
        }
        else if (g_current == Player::White) {
			WCHAR szText[256];
			LoadString(0, IDS_CPU_TURN, szText, _countof(szText));
            turnText = szText;
            textColor = D2D1::ColorF(0.1f, 0.1f, 0.1f);
        }

        // 描画矩形 (ウィンドウ下部中央に固定)
        D2D1_RECT_F textRect = D2D1::RectF(
            0.0f,
            g_windowH - (WINDOW_H_BASE - BOARD_PX_BASE) * g_current_dpi_scale + 5.0f,
            g_windowW,
            g_windowH
        );

        ID2D1SolidColorBrush* currentTextBrush = nullptr;
        if (SUCCEEDED(g_pRT->CreateSolidColorBrush(textColor, &currentTextBrush))) {
            g_pRT->DrawText(
                turnText,
                (UINT32)wcslen(turnText),
                g_pTextFormat,
                textRect,
                currentTextBrush
            );
            SAFE_RELEASE(currentTextBrush);
        }
    }
    // ------------------------------------

    DrawThinkingOverlay();

    g_pRT->EndDraw();
    EndPaint(hwnd, &ps);
}

// -------------------- Game Logic --------------------
bool InBoard(int x, int y) { return x >= 0 && y >= 0 && x < BOARD_SIZE && y < BOARD_SIZE; }

// 盤面クリアのみを行う関数 (リセットアニメーション完了時に使用)
void ResetGameImmediate() {
    for (int y = 0; y < BOARD_SIZE; y++) for (int x = 0; x < BOARD_SIZE; x++) g_board[y][x] = Player::Empty;
    g_current = Player::Black; g_gameOver = false; g_winLine = {}; g_history.clear(); g_anim.active = false;
    g_anim.reset_anim = false;
    g_aiThinking = false;
    if (g_hwndMain) { 
		WCHAR szTitle[256];
		LoadString(0, IDS_APP_TITLE, szTitle, _countof(szTitle));
        SetWindowText(g_hwndMain, szTitle);
    }
}

// Rキーによるリセット時などに呼ばれる関数 (アニメーション開始)
void ResetGame() {
    if (g_aiThinking.load()) return;

    bool has_stones = false;
    for (int y = 0; y < BOARD_SIZE; y++) for (int x = 0; x < BOARD_SIZE; x++) if (g_board[y][x] != Player::Empty) { has_stones = true; break; }

    if (has_stones) {
        // 盤面に石がある場合、リセットアニメーションを開始
        g_gameOver = false; // プレイ中状態に戻す
        g_winLine = {};
        g_aiThinking = false;

        // リセットアニメーション (0.1秒)
        // x, y, pはダミー値 (全石が対象)
        g_anim = { true, 0, 0, Player::Empty, NowSec(), 0.1, false, true };
    }
    else {
        // 石がない場合は即座にリセット
        ResetGameImmediate();
    }
}

bool CheckDirection(const BoardType board, int x, int y, int dx, int dy, Player p, int& cnt, int& x1, int& y1, int& x2, int& y2) {
    cnt = 1; int a = x, b = y; int c = x, d = y;
    for (int i = 1; i < 5; i++) { int nx = x + dx * i, ny = y + dy * i; if (!InBoard(nx, ny) || board[ny][nx] != p) break; cnt++; c = nx; d = ny; }
    for (int i = 1; i < 5; i++) { int nx = x - dx * i, ny = y - dy * i; if (!InBoard(nx, ny) || board[ny][nx] != p) break; cnt++; a = nx; b = ny; }
    x1 = a; y1 = b; x2 = c; y2 = d; return cnt >= 5;
}

bool CheckWin(const BoardType board, int x, int y, Player p, WinLine& out) {
    const int dirs[4][2] = { {1,0},{0,1},{1,1},{1,-1} };
    for (auto& d : dirs) {
        int cnt, x1, y1, x2, y2;
        if (CheckDirection(board, x, y, d[0], d[1], p, cnt, x1, y1, x2, y2)) {
            out = { true,x1,y1,x2,y2 };
            return true;
        }
    }
    return false;
}

bool PlaceStone(int x, int y, Player p, bool animate) {
    if (!InBoard(x, y) || g_board[y][x] != Player::Empty || g_gameOver) return false;

    g_anim.active = false;
    g_anim.reset_anim = false;

    g_board[y][x] = p;
    g_history.push_back({ x,y,p });
    if (animate) {
        // 設置アニメーション (0.20秒)
        g_anim = { true,x,y,p, NowSec(), 0.20, false, false };
    }

    WinLine currentWinLine = {};
    if (CheckWin(g_board, x, y, p, currentWinLine)) {
        g_winLine = currentWinLine;
        g_gameOver = true;
        g_aiThinking = false;
		WCHAR szTitle[256];
		LoadString(0, IDS_APP_TITLE, szTitle, _countof(szTitle));
        SetWindowText(g_hwndMain, szTitle);
    }
    return true;
}

// Undoの修正: 2手(AI->人間)を戻し、最後の石を消滅アニメーションさせる
void Undo() {
    if (g_history.empty() || g_gameOver || g_aiThinking.load()) return;

    int k = 0;

    // 戻す手数: 常にユーザーの番まで (最大2手: AI + 人間)
    if (g_history.back().p == Player::White) {
        // 直前がAIの手(White)の場合、2手戻す (AI, 人間)
        k = std::min<int>(2, (int)g_history.size());
    }
    else {
        // 直前が人間の手(Black)の場合、1手戻す (人間)
        k = 1;
    }

    // アニメーションのために、直前の石の情報を保存
    if (k > 0) {
        auto m = g_history.back();
        // 消滅アニメーション (0.15秒)
        g_anim = { true, m.x, m.y, m.p, NowSec(), 0.15, true, false };
    }

    // 盤面と履歴を更新
    for (int i = 0; i < k; i++) {
        if (g_history.empty()) break;
        auto m = g_history.back();
        g_history.pop_back();
        g_board[m.y][m.x] = Player::Empty;
    }

    g_gameOver = false;
    g_winLine = {};
    g_current = Player::Black; // Undo後は必ずユーザーの手番 (黒石)
}

// -------------------- Strong AI --------------------
struct ScoreMove { int x, y; int score; };
static std::array<uint64_t, BOARD_SIZE* BOARD_SIZE * 2> ZKEYS;
static bool g_zinit = false;
static uint64_t g_hash = 0;

inline int idxZ(int x, int y, Player p) {
    return (y * BOARD_SIZE + x) * 2 + ((p == Player::Black) ? 0 : 1);
}
void ZobristInit() {
    if (g_zinit) return;
    std::mt19937_64 rng(0xDEADBEEF12345678ULL);
    for (auto& k : ZKEYS) k = rng();
    g_hash = 0;
    g_zinit = true;
}
void ZApply(int x, int y, Player p) {
    g_hash ^= ZKEYS[idxZ(x, y, p)];
}

enum class TTFlag : uint8_t { Exact, Lower, Upper };
struct TTEntry { int depth; int eval; TTFlag flag; int bx, by; };
static std::unordered_map<uint64_t, TTEntry> TT;

static const int WIN_SCORE = 1'000'000, W_OPEN4 = 60'000, W_CLOSED4 = 12'000, W_OPEN3 = 6'000, W_CLOSED3 = 900, W_OPEN2 = 250, W_CLOSED2 = 60;

inline bool InBoardFast(int x, int y) {
    return (unsigned)x < (unsigned)BOARD_SIZE && (unsigned)y < (unsigned)BOARD_SIZE;
}

int EvalLine(const BoardType board, int x, int y, int dx, int dy, Player p) {
    int px = x - dx, py = y - dy;
    if (InBoardFast(px, py) && board[py][px] == p) return 0;
    int i = x, j = y;
    if (!InBoardFast(i, j) || board[j][i] != p) return 0;
    int cnt = 0;
    while (InBoardFast(i, j) && board[j][i] == p) {
        cnt++;
        i += dx;
        j += dy;
    }
    int open = 0;
    if (InBoardFast(i, j) && board[j][i] == Player::Empty) open++;
    if (InBoardFast(px, py) && board[py][px] == Player::Empty) open++;
    if (cnt >= 5) return WIN_SCORE;
    if (cnt == 4) return (open == 2 ? W_OPEN4 : (open == 1 ? W_CLOSED4 : 0));
    if (cnt == 3) return (open == 2 ? W_OPEN3 : (open == 1 ? W_CLOSED3 : 0));
    if (cnt == 2) return (open == 2 ? W_OPEN2 : (open == 1 ? W_CLOSED2 : 0));
    return 0;
}

int EvaluateBoard(const BoardType board, Player p) {
    Player q = (p == Player::Black) ? Player::White : Player::Black;
    int sp = 0, sq = 0;
    const int dirs[4][2] = { {1,0},{0,1},{1,1},{1,-1} };
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (board[y][x] == p) {
                for (auto& d : dirs) sp += EvalLine(board, x, y, d[0], d[1], p);
            }
            else if (board[y][x] == q) {
                for (auto& d : dirs) sq += EvalLine(board, x, y, d[0], d[1], q);
            }
        }
    }
    return sp - sq;
}

struct SearchCtx { double endTime; bool timeout = false; int nodes = 0; };
bool IsTimeUp(const SearchCtx& ctx) {
    return NowSec() >= ctx.endTime;
}

void GenMoves(const BoardType board, Player toMove, std::vector<ScoreMove>& out) {
    bool any = false;
    out.clear();

    BoardType temp_board;
    std::copy(&board[0][0], &board[0][0] + BOARD_SIZE * BOARD_SIZE, &temp_board[0][0]);
    WinLine dummy_winLine = {};

    auto nearStone = [&](int X, int Y) {
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                int nx = X + dx, ny = Y + dy;
                if (!InBoardFast(nx, ny)) continue;
                if (board[ny][nx] != Player::Empty) return true;
            }
        return false;
        };

    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (board[y][x] != Player::Empty) {
                any = true;
                break;
            }
        }
        if (any) break;
    }

    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (board[y][x] == Player::Empty && (!any || nearStone(x, y))) {
                temp_board[y][x] = toMove;
                bool win = CheckWin(temp_board, x, y, toMove, dummy_winLine);

                int s = win ? WIN_SCORE : EvaluateBoard(temp_board, toMove);
                out.push_back({ x,y,s });

                temp_board[y][x] = Player::Empty;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const ScoreMove& a, const ScoreMove& b) {
        return a.score > b.score;
        });
    if (out.size() > 64) out.resize(64);
}

int Negamax(BoardType board, int depth, int alpha, int beta, Player toMove, SearchCtx& ctx, int& bestx, int& besty) {
    if (IsTimeUp(ctx)) {
        ctx.timeout = true;
        return 0;
    }

    auto it = TT.find(g_hash);
    if (it != TT.end() && it->second.depth >= depth) {
        const TTEntry& e = it->second;
        bestx = e.bx;
        besty = e.by;
        if (e.flag == TTFlag::Exact) return e.eval;
        else if (e.flag == TTFlag::Lower) alpha = max(alpha, e.eval);
        else if (e.flag == TTFlag::Upper) beta = min(beta, e.eval);
        if (alpha >= beta) return e.eval;
    }

    if (depth == 0) return EvaluateBoard(board, toMove);

    std::vector<ScoreMove> moves;
    GenMoves(board, toMove, moves);

    if (moves.empty()) return 0;

    int localBestX = -1, localBestY = -1;
    int bestVal = -2000000000;

    WinLine dummy_winLine = {};

    for (const auto& m : moves) {
        board[m.y][m.x] = toMove;
        ZApply(m.x, m.y, toMove);

        bool win = CheckWin(board, m.x, m.y, toMove, dummy_winLine);
        int val;

        if (win) {
            val = WIN_SCORE;
        }
        else {
            Player opp = (toMove == Player::Black) ? Player::White : Player::Black;
            int bx = -1, by = -1;
            val = -Negamax(board, depth - 1, -beta, -alpha, opp, ctx, bx, by);

            if (ctx.timeout) {
                board[m.y][m.x] = Player::Empty;
                ZApply(m.x, m.y, toMove);
                return 0;
            }
        }

        board[m.y][m.x] = Player::Empty;
        ZApply(m.x, m.y, toMove);

        if (val > bestVal) {
            bestVal = val;
            localBestX = m.x;
            localBestY = m.y;
        }

        if (val > alpha) {
            alpha = val;
        }

        if (alpha >= beta) break;
    }

    TTEntry entry;
    entry.depth = depth;
    entry.eval = bestVal;
    entry.bx = localBestX;
    entry.by = localBestY;

    if (bestVal <= alpha) entry.flag = TTFlag::Upper;
    else if (bestVal >= beta) entry.flag = TTFlag::Lower;
    else entry.flag = TTFlag::Exact;

    TT[g_hash] = entry;
    bestx = localBestX;
    besty = localBestY;

    return bestVal;
}

static int MAX_DEPTH = 4; // 強さ調整
static int MAX_SEARCH_MS = 800; // 思考時間(ms)

void AIWorker() {
    ZobristInit();
    Player me = Player::White;

    BoardType local_board;
    std::copy(&g_board[0][0], &g_board[0][0] + BOARD_SIZE * BOARD_SIZE, &local_board[0][0]);

    bool any = false;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (local_board[y][x] != Player::Empty) {
                any = true;
                break;
            }
        }
        if (any) break;
    }

    int rx = -1, ry = -1;
    if (!any) {
        rx = (BOARD_SIZE - 1) / 2; ry = (BOARD_SIZE - 1) / 2;
    }
    else {
        SearchCtx ctx;
        ctx.endTime = NowSec() + (MAX_SEARCH_MS / 1000.0);
        ctx.timeout = false;
        ctx.nodes = 0;

        int alpha = -1500000000, beta = 1500000000;
        int lastBestX = -1, lastBestY = -1;

        for (int depth = 2; depth <= MAX_DEPTH; ++depth) {
            int bx = -1, by = -1;
            (void)Negamax(local_board, depth, alpha, beta, me, ctx, bx, by);

            if (!ctx.timeout && bx != -1) {
                lastBestX = bx;
                lastBestY = by;
            }
            if (ctx.timeout) break;
        }

        if (lastBestX == -1) {
            std::vector<ScoreMove> cand;
            GenMoves(local_board, me, cand);
            if (!cand.empty()) {
                lastBestX = cand.front().x;
                lastBestY = cand.front().y;
            }
        }
        rx = lastBestX; ry = lastBestY;
    }

    PostMessage(g_hwndMain, WM_APP_AIMOVE, 0, MAKELPARAM(rx, ry));
}

void StartAIThink() {
    if (g_aiThinking.load() || g_gameOver) return;
    g_aiThinking = true;
    g_hash = 0;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (g_board[y][x] != Player::Empty) {
                ZApply(x, y, g_board[y][x]);
            }
        }
    }

	WCHAR szTitle[256];
	LoadString(0, IDS_APP_TITLE, szTitle, _countof(szTitle));
	WCHAR szThinking[256];
	LoadString(0, IDS_THINKING, szThinking, _countof(szThinking));
	lstrcatW(szTitle, szThinking);
    SetWindowText(g_hwndMain, szTitle);
    std::thread(AIWorker).detach();
}

// -------------------- Input Helpers --------------------
// マウス入力位置(px, py)を盤面座標(outx, outy)に変換する関数
bool HitToCell(int px, int py, int& outx, int& outy) {
    // スケーリングとオフセットを考慮してピクセル座標をボード座標に逆変換
    float fx = (float)px;
    float fy = (float)py;

    // オフセットを差し引く
    fx -= g_offsetX;
    fy -= g_offsetY;

    // スケールで割る
    fx /= g_current_cell_size;
    fy /= g_current_cell_size;

    // 最も近い格子点を決定
    int x = (int)std::round(fx);
    int y = (int)std::round(fy);

    if (!InBoard(x, y)) return false;

    // 最後に、クリックした点がグリッド交点から許容範囲内にあるかチェック
    float dx = (float)x - fx;
    float dy = (float)y - fy;
    float d2 = dx * dx + dy * dy;

    // ボード座標系での許容範囲の二乗
    const float HIT_TOLERANCE_SQUARED = 0.40f * 0.40f;

    if (d2 > HIT_TOLERANCE_SQUARED) return false;

    outx = x;
    outy = y;
    return true;
}

// -------------------- WinMain --------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // DPI認識の設定
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 初期DPIスケールを取得
    UINT dpi = GetDpiForSystem();
    g_current_dpi_scale = (float)dpi / 96.0f;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pFactory))) return 0;
    const wchar_t CLASS_NAME[] = L"GomokuWnd";
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_GOMOKU));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

	WCHAR szTitle[256];
	LoadString(0, IDS_APP_TITLE, szTitle, _countof(szTitle));
    HWND hwnd = CreateWindowEx(0, CLASS_NAME, szTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_W_BASE, WINDOW_H_BASE, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;
    g_hwndMain = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 初期サイズ計算を強制
    RECT rc; GetClientRect(hwnd, &rc);
    OnResize(rc.right - rc.left, rc.bottom - rc.top);

    ResetGameImmediate(); // 初期起動時は即座にリセット
    SetTimer(hwnd, 1, TIMER_FPS, nullptr);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    SAFE_RELEASE(g_pFactory);
    return 0;
}

void TriggerRepaint(HWND hwnd) { InvalidateRect(hwnd, nullptr, FALSE); }

// ゲーム終了時に勝敗に応じたダイアログを表示し、リセットを確認するヘルパー関数
void ShowResetDialogWithResult(HWND hwnd) {
    if (!g_gameOver) return;

    // 最後に石を置いたプレイヤーを特定
    Player winner = g_history.empty() ? Player::Empty : g_history.back().p;

    WCHAR msg_win[256];
	WCHAR msg_lose[256];
	WCHAR msg_general[256];

	LoadString(0, IDS_YOU_WIN, msg_win, _countof(msg_win));
	LoadString(0, IDS_YOU_LOSE, msg_lose, _countof(msg_lose));
	LoadString(0, IDS_GAME_OVER_RESET, msg_general, _countof(msg_general));

    const wchar_t* msg = msg_general;
    if (winner == Player::Black) {
        msg = msg_win;
    }
    else if (winner == Player::White) {
        msg = msg_lose;
    }

	WCHAR szResult[128];
	LoadString(0, IDS_RESULT, szResult, _countof(szResult));

    int res = MessageBox(hwnd, msg, szResult, MB_YESNO | MB_ICONQUESTION);
    if (res == IDYES) {
        ResetGameImmediate();
        TriggerRepaint(hwnd);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        OnResize(LOWORD(lParam), HIWORD(lParam));
        TriggerRepaint(hwnd);
        return 0;

    case WM_DPICHANGED: { // DPI変更イベントの処理
        UINT newDpi = (UINT)HIWORD(wParam);
        float newDpiScale = (float)newDpi / 96.0f;

        if (newDpiScale != g_current_dpi_scale) {
            g_current_dpi_scale = newDpiScale;
            // DirectWriteリソースを新しいDPIで再生成
            CreateTextFormat(g_current_dpi_scale);
        }

        // ウィンドウサイズと位置の調整
        const RECT* prcNewWindow = (RECT*)lParam;
        SetWindowPos(hwnd,
            NULL,
            prcNewWindow->left,
            prcNewWindow->top,
            prcNewWindow->right - prcNewWindow->left,
            prcNewWindow->bottom - prcNewWindow->top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        return 0;
    }

    case WM_TIMER:
        if (wParam == 1) {
            // アニメーションの終了判定
            if (g_anim.active) {
                double t = (NowSec() - g_anim.t0) / g_anim.dur;
                if (t >= 1.0) {
                    // アニメーションが終了した場合
                    if (g_anim.reset_anim) {
                        // リセットアニメーションの場合、盤面をクリアし、アニメーションフラグをリセット
                        ResetGameImmediate();
                    }
                    else {
                        // 通常またはUndoアニメーションの場合、単にアクティブフラグを解除
                        g_anim.active = false;
                    }
                }
            }
            TriggerRepaint(hwnd);
        }
        return 0;
    case WM_LBUTTONUP: {
        // g_gameOverなら、盤面クリックで確認ダイアログを表示する
        if (g_gameOver) {
            ShowResetDialogWithResult(hwnd);
            return 0;
        }

        if (g_aiThinking.load()) return 0;
        if (g_current != Player::Black) return 0;

        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        int x, y;

        if (HitToCell(mx, my, x, y)) {
            if (PlaceStone(x, y, Player::Black, true)) {
                if (g_gameOver) {
                    // プレイヤーの手でゲームが終了した場合、勝敗ダイアログを表示
                    ShowResetDialogWithResult(hwnd);
                }
                else {
                    g_current = Player::White;
                    StartAIThink();
                }
            }
            TriggerRepaint(hwnd);
        }
        return 0;
    }
    case WM_RBUTTONUP:
        // Undoは常に2手(または1手)戻し、アニメーションを追加
        if (!g_aiThinking.load()) {
            Undo();
            TriggerRepaint(hwnd);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == 'R' || wParam == 'r') {
            // Rキーリセットはアニメーションを開始
            if (!g_aiThinking.load()) {
                ResetGame(); // アニメーション開始
                TriggerRepaint(hwnd);
            }
        }
        return 0;
    case WM_APP_AIMOVE: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        bool stonePlaced = false;
        if (x >= 0 && y >= 0 && !g_gameOver) {
            stonePlaced = PlaceStone(x, y, Player::White, true);
        }

        g_aiThinking = false;

        WCHAR szTitle[256];
        LoadString(0, IDS_APP_TITLE, szTitle, _countof(szTitle));
        SetWindowText(g_hwndMain, szTitle);

        if (g_gameOver && stonePlaced) {
            // AIの手でゲームが終了した場合、勝敗ダイアログを表示
            ShowResetDialogWithResult(hwnd);
        }
        else if (!g_gameOver) {
            g_current = Player::Black;
        }
        TriggerRepaint(hwnd);
        return 0;
    }
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        DiscardGraphicsResources();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}