/*
 * Win32 editor view: a child window of the host's HWND, painted from the same
 * canvas with GDI, with mouse and keyboard forwarded to the ui_t.
 */
#include "cdmvst.h"

#include <windows.h>
#include <windowsx.h>

#include <atomic>
#include <cstring>

namespace cdm {
namespace {

using namespace Steinberg;

constexpr const wchar_t *kClass = L"CircularDrumMachineView";

class WinView : public IPlugView {
public:
    explicit WinView (host *h) : h_ (h) { size_ = ViewRect (0, 0, CDM_VIEW_W, CDM_VIEW_H); }

    ~WinView () { detach (); }

    //--- FUnknown ---------------------------------------------------------
    tresult PLUGIN_API queryInterface (const TUID iid, void **obj) override
    {
        if (!obj)
            return kInvalidArgument;
        QUERY_INTERFACE (iid, obj, FUnknown::iid, IPlugView)
        QUERY_INTERFACE (iid, obj, IPlugView::iid, IPlugView)
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return (uint32)++ref_; }
    uint32 PLUGIN_API release () override
    {
        const int n = --ref_;
        if (n == 0) {
            delete this;
            return 0;
        }
        return (uint32)n;
    }

    //--- IPlugView --------------------------------------------------------
    tresult PLUGIN_API isPlatformTypeSupported (FIDString type) override
    {
        return type && std::strcmp (type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API attached (void *parent, FIDString type) override
    {
        if (isPlatformTypeSupported (type) != kResultTrue || !parent)
            return kResultFalse;
        if (wnd_)
            return kResultOk;

        WNDCLASSEXW wc {};
        wc.cbSize = sizeof wc;
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &WinView::proc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW (&wc); /* harmless if it already exists */

        parent_ = (HWND)parent;
        wnd_ = CreateWindowExW (0, kClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, CDM_VIEW_W,
                                CDM_VIEW_H, parent_, nullptr, wc.hInstance, this);
        if (!wnd_)
            return kResultFalse;

        SetWindowLongPtrW (wnd_, GWLP_USERDATA, (LONG_PTR)this);
        SetTimer (wnd_, 1, 16, nullptr);
        return kResultOk;
    }

    tresult PLUGIN_API removed () override
    {
        detach ();
        return kResultOk;
    }

    tresult PLUGIN_API onWheel (float distance) override
    {
        if (h_->ui)
            ui_wheel (h_->ui, distance, (float)last_x_, (float)last_y_);
        return kResultOk;
    }

    tresult PLUGIN_API onKeyDown (char16, int16, int16) override { return kResultOk; }
    tresult PLUGIN_API onKeyUp (char16, int16, int16) override { return kResultOk; }

    tresult PLUGIN_API getSize (ViewRect *size) override
    {
        if (!size)
            return kInvalidArgument;
        *size = size_;
        return kResultOk;
    }

    tresult PLUGIN_API onSize (ViewRect *newSize) override
    {
        if (!newSize)
            return kInvalidArgument;
        size_ = *newSize;
        if (wnd_)
            SetWindowPos (wnd_, nullptr, 0, 0, size_.getWidth (), size_.getHeight (),
                          SWP_NOZORDER | SWP_NOMOVE);
        return kResultOk;
    }

    tresult PLUGIN_API onFocus (TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame (IPlugFrame *frame) override
    {
        frame_ = frame;
        return kResultOk;
    }
    tresult PLUGIN_API canResize () override { return kResultFalse; }
    tresult PLUGIN_API checkSizeConstraint (ViewRect *rect) override
    {
        if (!rect)
            return kInvalidArgument;
        rect->right = rect->left + CDM_VIEW_W;
        rect->bottom = rect->top + CDM_VIEW_H;
        return kResultTrue;
    }

private:
    void detach ()
    {
        if (wnd_) {
            KillTimer (wnd_, 1);
            DestroyWindow (wnd_);
            wnd_ = nullptr;
        }
        parent_ = nullptr;
    }

    void paint (HDC dc)
    {
        canvas_t *c = h_->canvas;
        if (!c)
            return;
        BITMAPINFO bmi {};
        bmi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = c->w;
        bmi.bmiHeader.biHeight = -c->h; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits (dc, 0, 0, c->w, c->h, 0, 0, c->w, c->h, c->px, &bmi, DIB_RGB_COLORS,
                       SRCCOPY);
    }

    void tick (float dt)
    {
        if (!h_->ui || !h_->canvas)
            return;
        ui_tick (h_->ui, dt);
        if (ui_needs_frame (h_->ui)) {
            ui_frame (h_->ui, h_->canvas);
            if (wnd_)
                InvalidateRect (wnd_, nullptr, FALSE);
        }
    }

    static LRESULT CALLBACK proc (HWND hwnd, UINT msg, WPARAM w, LPARAM l)
    {
        WinView *self = (WinView *)GetWindowLongPtrW (hwnd, GWLP_USERDATA);
        if (!self)
            return DefWindowProcW (hwnd, msg, w, l);

        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint (hwnd, &ps);
                self->paint (dc);
                EndPaint (hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1; /* the canvas covers every pixel */
            case WM_TIMER:
                self->tick (0.016f);
                return 0;
            case WM_LBUTTONDOWN:
                self->last_x_ = GET_X_LPARAM (l);
                self->last_y_ = GET_Y_LPARAM (l);
                SetFocus (hwnd);
                if (self->h_->ui)
                    ui_pointer (self->h_->ui, UI_DOWN, (float)self->last_x_,
                                (float)self->last_y_, 1);
                return 0;
            case WM_LBUTTONUP:
                if (self->h_->ui)
                    ui_pointer (self->h_->ui, UI_UP, (float)GET_X_LPARAM (l),
                                (float)GET_Y_LPARAM (l), 1);
                return 0;
            case WM_MOUSEMOVE:
                self->last_x_ = GET_X_LPARAM (l);
                self->last_y_ = GET_Y_LPARAM (l);
                if (self->h_->ui)
                    ui_pointer (self->h_->ui, UI_MOVE, (float)self->last_x_,
                                (float)self->last_y_, 0);
                return 0;
            case WM_MOUSEWHEEL: {
                const int delta = GET_WHEEL_DELTA_WPARAM (w) / WHEEL_DELTA;
                if (self->h_->ui)
                    ui_wheel (self->h_->ui, (float)delta, (float)self->last_x_,
                              (float)self->last_y_);
                return 0;
            }
            case WM_KEYDOWN: {
                int key = 0;
                switch (w) {
                    case VK_RETURN: key = 13; break;
                    case VK_ESCAPE: key = 27; break;
                    case VK_BACK: key = 8; break;
                    case VK_SPACE: key = ' '; break;
                    default: return DefWindowProcW (hwnd, msg, w, l);
                }
                if (self->h_->ui)
                    ui_key (self->h_->ui, key, 1);
                return 0;
            }
            case WM_CHAR: {
                char text[2] = {(char)w, 0};
                if (self->h_->ui)
                    ui_text (self->h_->ui, text);
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW (hwnd, msg, w, l);
    }

    host *h_ {};
    IPlugFrame *frame_ = nullptr;
    ViewRect size_ {};
    HWND parent_ = nullptr;
    HWND wnd_ = nullptr;
    int last_x_ = 0, last_y_ = 0;
    std::atomic<int> ref_ {1};
};

} /* namespace */

IPlugView *create_platform_view (host *h)
{
    return new WinView (h);
}

} /* namespace cdm */
