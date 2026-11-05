/*
 * Circular Drum Machine - standalone shell.
 *
 * Owns the window, the GL context and the event loop. The interface is drawn
 * in software into a 1000x720 buffer and presented as a single textured quad,
 * so the standalone and (later) the VST3 editor share one drawing path.
 *
 *   ./circular_drum_machine                 run
 *   ./circular_drum_machine --dump out.png   render one frame headlessly
 */
#include <SDL.h>
#include <GL/glew.h>

#include <png.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "draw.h"
#include "ui.h"

#define W 880
#define H 500
#define TITLE "Circular Drum Machine"

static int write_png(const canvas_t *c, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return 0;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return 0;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return 0;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return 0;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)c->w, (png_uint_32)c->h, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    uint8_t *row = malloc((size_t)c->w * 4);
    if (!row) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return 0;
    }
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[(size_t)y * (size_t)c->w + x];
            row[x * 4 + 0] = (uint8_t)((p >> 16) & 0xff);
            row[x * 4 + 1] = (uint8_t)((p >> 8) & 0xff);
            row[x * 4 + 2] = (uint8_t)(p & 0xff);
            row[x * 4 + 3] = 0xff;
        }
        png_write_row(png, row);
    }
    free(row);

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 1;
}

static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    frag = texture(u_tex, v_uv);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint make_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
    if (!vs || !fs)
        return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "link: %s\n", log);
        prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static void present(const canvas_t *c, GLuint tex, GLuint prog, GLuint vao)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c->w, c->h, GL_BGRA, GL_UNSIGNED_BYTE,
                    c->px);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static int run(canvas_t *canvas, ui_t *ui, mixer_t *mix)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *win = SDL_CreateWindow(TITLE, SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, W, H,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        fprintf(stderr, "gl: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "glew: %s\n", (const char *)glewGetErrorString(glewInit()));
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    while (glGetError() != GL_NO_ERROR) {
    }

    GLuint prog = make_program();
    if (!prog) {
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    glViewport(0, 0, W, H);
    SDL_GL_SetSwapInterval(1);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    static const float quad[] = {
        -1.0f, 1.0f,  0.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
        1.0f,  -1.0f, 1.0f, 1.0f,
    };
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);

    SDL_StartTextInput();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    audio_t *aud = audio_open(mix);
    if (!aud)
        fprintf(stderr, "audio: running without an output device\n");

    int running = 1;
    int redraw = 1;
    /* Pointer as last seen over the window; SDL2 drop events carry no position. */
    float ptr_x = -1.0f, ptr_y = -1.0f;
    uint64_t prev = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN: {
                SDL_Keycode k = ev.key.keysym.sym;
                int key = (k == SDLK_SPACE) ? ' ' : (int)k;
                /* ui_key claims esc while a dialog is open. */
                if (ui_key(ui, key, 1))
                    redraw = 1;
                else if (k == SDLK_ESCAPE)
                    running = 0;
                break;
            }
            case SDL_KEYUP: {
                SDL_Keycode k = ev.key.keysym.sym;
                int key = (k == SDLK_SPACE) ? ' ' : (int)k;
                redraw |= ui_key(ui, key, 0);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                ptr_x = (float)ev.button.x;
                ptr_y = (float)ev.button.y;
                redraw |= ui_pointer(ui, UI_DOWN, ptr_x, ptr_y, ev.button.button);
                break;
            case SDL_MOUSEBUTTONUP:
                redraw |= ui_pointer(ui, UI_UP, (float)ev.button.x, (float)ev.button.y,
                                     ev.button.button);
                break;
            case SDL_MOUSEMOTION:
                ptr_x = (float)ev.motion.x;
                ptr_y = (float)ev.motion.y;
                redraw |= ui_pointer(ui, UI_MOVE, ptr_x, ptr_y, 0);
                break;
            case SDL_TEXTINPUT:
                redraw |= ui_text(ui, ev.text.text);
                break;
            case SDL_DROPFILE: {
                /* SDL2's drop event carries no coordinates: take the pointer as
                 * last seen over the window, else ask X where it is. */
                float dx = ptr_x, dy = ptr_y;
                if (dx < 0.0f || dy < 0.0f) {
                    int mx = 0, my = 0;
                    SDL_GetMouseState(&mx, &my);
                    dx = (float)mx;
                    dy = (float)my;
                }
                redraw |= ui_drop(ui, dx, dy, ev.drop.file);
                SDL_free(ev.drop.file);
                break;
            }
            case SDL_MOUSEWHEEL:
                {
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    redraw |= ui_wheel(ui, (float)ev.wheel.y, (float)mx, (float)my);
                }
                break;
            default:
                break;
            }
        }

        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now - prev) / (double)SDL_GetPerformanceFrequency());
        prev = now;
        if (dt > 0.1f)
            dt = 0.1f;

        ui_tick(ui, dt);
        if (ui_needs_frame(ui))
            redraw = 1;

        if (redraw) {
            ui_frame(ui, canvas);
            present(canvas, tex, prog, vao);
            SDL_GL_SwapWindow(win);
            redraw = 0;
        }
        SDL_Delay(1);
    }

    audio_close(aud);

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    const char *dump = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--dump out.png]\n", argv[0]);
            return 2;
        }
    }

    canvas_t *canvas = canvas_new(W, H);
    ui_t *ui = ui_create();
    if (!canvas || !ui) {
        fprintf(stderr, "startup failed\n");
        canvas_free(canvas);
        ui_destroy(ui);
        return 1;
    }

    if (dump) {
        ui_frame(ui, canvas);
        int ok = write_png(canvas, dump);
        canvas_free(canvas);
        ui_destroy(ui);
        if (!ok) {
            fprintf(stderr, "could not write %s\n", dump);
            return 1;
        }
        printf("wrote %s (%dx%d)\n", dump, W, H);
        return 0;
    }

    mixer_t *mix = mixer_new(48000); /* matches the device request in audio.c */
    if (!mix) {
        fprintf(stderr, "startup failed\n");
        canvas_free(canvas);
        ui_destroy(ui);
        return 1;
    }
    ui_set_mixer(ui, mix);

    int rc = run(canvas, ui, mix);
    canvas_free(canvas);
    ui_destroy(ui);
    mixer_free(mix);
    return rc;
}
