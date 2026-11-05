#ifndef CDM_VST3_H
#define CDM_VST3_H

/*
 * Shared between the plugin and its two editor backends. The editor draws the
 * same 880x500 software canvas the standalone uses and forwards input to the
 * very same ui_t, so the machine behaves identically inside a host.
 */

#include "pluginterfaces/gui/iplugview.h"

#include "audio.h"
#include "ui.h"

#define CDM_VIEW_W 880
#define CDM_VIEW_H 500

namespace cdm {

struct host {
    ui_t *ui;
    canvas_t *canvas;
    mixer_t *mixer;
    void *plugin; /* the VST3 component, for editor<->state calls */
};

/* Implemented once per platform: view_x11.cpp / view_win.cpp. */
Steinberg::IPlugView *create_platform_view(host *h);

} /* namespace cdm */

#endif
