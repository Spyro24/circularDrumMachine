/*
 * X11 editor view: the machine's canvas drawn straight into the window the
 * host hands over (X11EmbedWindowID), with mouse and keyboard forwarded to
 * the same ui_t the standalone drives.
 */
#include "cdmvst.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace cdm {
namespace {

using namespace Steinberg;

class X11View : public IPlugView {
public:
    explicit X11View (host *h) : h_ (h) { size_ = ViewRect (0, 0, CDM_VIEW_W, CDM_VIEW_H); }

    ~X11View () { detach (); }

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
        return type && std::strcmp (type, kPlatformTypeX11EmbedWindowID) == 0 ? kResultTrue
                                                                             : kResultFalse;
    }

    tresult PLUGIN_API attached (void *parent, FIDString type) override
    {
        if (isPlatformTypeSupported (type) != kResultTrue || !parent)
            return kResultFalse;
        if (dpy_)
            return kResultOk; // already attached

        dpy_ = XOpenDisplay (nullptr);
        if (!dpy_)
            return kResultFalse;

        win_ = (Window) (uintptr_t)parent;
        gc_ = XCreateGC (dpy_, win_, 0, nullptr);
        XSelectInput (dpy_, win_,
                      ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                          KeyPressMask | KeyReleaseMask | StructureNotifyMask);
        XFlush (dpy_);

        running_ = true;
        thread_ = std::thread (&X11View::loop, this);
        return kResultOk;
    }

    tresult PLUGIN_API removed () override
    {
        detach ();
        return kResultOk;
    }

    tresult PLUGIN_API onWheel (float distance) override
    {
        if (!h_->ui)
            return kResultOk;
        ui_wheel (h_->ui, distance, (float)last_x_, (float)last_y_);
        return kResultOk;
    }

    tresult PLUGIN_API onKeyDown (char16 /*key*/, int16 /*keyCode*/, int16 /*modifiers*/) override
    {
        return kResultOk;
    }
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
        return kResultOk;
    }

    tresult PLUGIN_API onFocus (TBool /*state*/) override { return kResultOk; }
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
        running_ = false;
        if (thread_.joinable ())
            thread_.join ();
        if (img_) {
            img_->data = nullptr; // the pixels belong to the canvas
            XDestroyImage (img_);
            img_ = nullptr;
        }
        if (gc_ && dpy_) {
            XFreeGC (dpy_, gc_);
            gc_ = nullptr;
        }
        if (dpy_) {
            XCloseDisplay (dpy_);
            dpy_ = nullptr;
        }
        win_ = 0;
    }

    void send_key (XKeyEvent &k)
    {
        char buf[8] = {0};
        KeySym sym = 0;
        XLookupString (&k, buf, sizeof buf - 1, &sym, nullptr);
        if (buf[0] && h_->ui) {
            if (!ui_text (h_->ui, buf))
                ui_key (h_->ui, (int) (unsigned char)buf[0], 1);
            return;
        }
        int key = 0;
        switch (sym) {
            case XK_Return: key = 13; break;
            case XK_Escape: key = 27; break;
            case XK_BackSpace: key = 8; break;
            case XK_space: key = ' '; break;
            default: return;
        }
        if (h_->ui)
            ui_key (h_->ui, key, 1);
    }

    void handle (XEvent &e)
    {
        if (!h_->ui)
            return;
        switch (e.type) {
            case ButtonPress:
                last_x_ = e.xbutton.x;
                last_y_ = e.xbutton.y;
                if (e.xbutton.button == Button1)
                    ui_pointer (h_->ui, UI_DOWN, (float)last_x_, (float)last_y_, 1);
                else if (e.xbutton.button == Button4)
                    ui_wheel (h_->ui, 1.0f, (float)last_x_, (float)last_y_);
                else if (e.xbutton.button == Button5)
                    ui_wheel (h_->ui, -1.0f, (float)last_x_, (float)last_y_);
                break;
            case ButtonRelease:
                if (e.xbutton.button == Button1)
                    ui_pointer (h_->ui, UI_UP, (float)e.xbutton.x, (float)e.xbutton.y, 1);
                break;
            case MotionNotify:
                last_x_ = e.xmotion.x;
                last_y_ = e.xmotion.y;
                ui_pointer (h_->ui, UI_MOVE, (float)last_x_, (float)last_y_, 0);
                break;
            case KeyPress:
                send_key (e.xkey);
                break;
            default:
                break;
        }
    }

    void present ()
    {
        canvas_t *c = h_->canvas;
        if (!c || !dpy_ || !win_)
            return;
        if (!img_) {
            img_ = XCreateImage (dpy_, DefaultVisual (dpy_, DefaultScreen (dpy_)),
                                 (unsigned)DefaultDepth (dpy_, DefaultScreen (dpy_)), ZPixmap, 0,
                                 (char *)c->px, (unsigned)c->w, (unsigned)c->h, 32, 0);
            if (!img_)
                return;
        }
        XPutImage (dpy_, win_, gc_, img_, 0, 0, 0, 0, (unsigned)c->w, (unsigned)c->h);
        XFlush (dpy_);
    }

    void loop ()
    {
        auto prev = std::chrono::steady_clock::now ();
        while (running_) {
            while (dpy_ && XPending (dpy_) > 0) {
                XEvent e;
                XNextEvent (dpy_, &e);
                handle (e);
            }

            auto now = std::chrono::steady_clock::now ();
            float dt = std::chrono::duration<float> (now - prev).count ();
            prev = now;
            if (dt > 0.1f)
                dt = 0.1f;

            if (h_->ui && h_->canvas) {
                ui_tick (h_->ui, dt);
                if (ui_needs_frame (h_->ui)) {
                    ui_frame (h_->ui, h_->canvas);
                    present ();
                }
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (8));
        }
    }

    host *h_ {};
    IPlugFrame *frame_ = nullptr;
    ViewRect size_ {};
    Display *dpy_ = nullptr;
    Window win_ = 0;
    GC gc_ = nullptr;
    XImage *img_ = nullptr;
    int last_x_ = 0, last_y_ = 0;
    std::atomic<bool> running_ {false};
    std::atomic<int> ref_ {1};
    std::thread thread_;
};

} /* namespace */

IPlugView *create_platform_view (host *h)
{
    return new X11View (h);
}

} /* namespace cdm */
