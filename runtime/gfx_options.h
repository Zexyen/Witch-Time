/* Track O (options), first set: config\graphics.ini. See gfx_options.c and plan\phases\O1_options.md. */
#ifndef GFX_OPTIONS_H
#define GFX_OPTIONS_H
typedef struct {
    char path[260];
    int renderer;            /* 0 Vulkan (default), 1 OpenGL                               env SSBU_RENDERER=vk|gl wins */
    int window_mode;         /* 0 windowed, 1 borderless (desktop size), 2 fullscreen (borderless, topmost, covering the monitor) */
    int width, height;       /* client size of the windowed mode */
    int present;             /* 0 clock (mailbox, else immediate, paced by our 60 Hz clock), 1 vsync on (fifo), 2 vsync off (immediate), 3 mailbox;   env SSBU_VK_PRESENT wins */
    char gpu[64];            /* "" = discrete first; an index of the printed list, or a part of the device name   env SSBU_VK_DEVICE wins */
    float scale;             /* internal resolution scale asked for (Vulkan: nvn_vk_draw.c rs_scale, 0.25 .. 2.0 since PR #8 (stephenjayakar); env SSBU_VK_SCALE wins) */
    int handheld;            /* 1: the game is told it runs handheld = it makes its own 1280x720 render targets instead of 1920x1080 */
    int aniso;               /* 0 = the game's own value; 2, 4, 8, 16 = at least that on samplers that filter between mip levels */
    int overlay;             /* 1: frame time in the title bar and a "frame-time:" log line every second */
    int shader_skip;         /* shader_wait: 0 hold (a draw waits for its shader or pipeline: the frame is late), 1 skip (near a fight the draw is left out until they are ready; nvn_vk_m4.c skip_on; env SSBU_VK_NOHOLD wins) */
    int bc7_textures;       /* #54: 1 (default) = textures from `wtbuild textures`' pack where it has them (nvn_gl.c pack_on); env SSBU_BC7_TEXTURES wins */
    int profile;            /* 0 custom (the keys as written), 1 low: scale, aniso and present are set by the profile (gfx_options.c) */
    int shader_prepare;     /* track V session 14 (appended: older objects keep their offsets): 0 first_run (the game is held at its first frame until every shader of the game's data is prepared, once per GPU + driver), 1 background (as before), 2 off;  env SSBU_VK_PREPARE wins */
    int aspect_stretch;     /* [Recomps#1] aspect = keep (0, default: the picture keeps its shape, black bars on a wider or taller
                               window, e.g. an ultrawide monitor) | stretch (1: it fills the window); nvn_vk_draw.c present (Vulkan) */
    int frame_rate;         /* [Recomps#1] frame_rate = 60 (the game as made) .. 120: above 60 the lifted frame-rate patch is on, capped there */
    int hdr;                /* [Recomps#1] hdr = auto (0: HDR output while Windows HDR is on for the window's monitor) | off (1) | on (2: always);
                               nvn_vk_draw.c hdr_* (Vulkan on Windows; finding 147) */
    int hdr_scrgb;          /* [Recomps#1] hdr_format = hdr10 (0: 10-bit PQ, BT.2020) | scrgb (1: 16-bit floating point, linear) */
    float hdr_paper_white;  /* [Recomps#1] hdr_paper_white: the game's white in nits (80 .. 1000); 0 = auto: Windows' SDR content brightness */
    int hdr_srgb;           /* [Recomps#1] hdr_gamma = 2.2 (0: the game's values read as an SDR monitor shows them) | srgb (1: as Windows shows SDR apps) */
    int cutscene_fps;       /* [witchtime] cutscene_fps = 30 (the game's 30 fps mode on the title, in menus and cutscenes, as on the console) | 60;
                             * env SSBU_CUTSCENE_FPS wins; read by game_shim_eh.c h_fps_mode_set through that variable */
    float texture_lod_bias; /* [witchtime] texture_lod_bias = 0 (the game's own) .. -3 (sharper: finer mip levels) .. +3; added to every sampler that uses
                             * mip maps (nvn_vk_draw.c sampler_get); env SSBU_TEXTURE_LOD_BIAS wins */
    float aspect_ratio;     /* [witchtime] aspect_ratio = 16:9 (0: the game's own) | auto (-1: the window's shape, gfx_aspect_window) | <w>:<h>: the shape of the game's screen, at the next start; handed on as
                             * RECOMPS_ASPECT (game_shim_eh.c, r2_imports.c, nvn_vk_draw.c rs_xstretch), which wins when set */
    int hud_centre;         /* [witchtime] hud_area = screen (0: wider than 16:9 the HUD's corners go to the screen's edges) | 16:9 (1: kept in the middle);
                             * handed on as RECOMPS_HUD_CENTRE=1 (game_shim_eh.c h_ui_element) */
} GfxOptions;
const GfxOptions *gfx_options(void);      /* any thread; read once (the file is written with defaults when it does not exist) */
int gfx_pace_rate(void);                  /* [Recomps#1] the rate display.c paces the game at: 60, or frame_rate above 60 (SSBU_PACE_120=0: 60 and the lifted limiter) */
int gfx_ini_refresh(const char *path);                   /* [witchtime] a new version's comments and keys in the player's file, values kept; 1 = rewritten */
void gfx_aspect_window(int w, int h);     /* [witchtime] aspect_ratio = auto: the window's client size, before the game starts (display.c) */
int gfx_ini_get(const char *key, char *out, size_t n);      /* [witchtime] graphics.ini's value of a key as the reader sees it; 0 = not in the file */
int gfx_option_set(const char *key, const char *val);     /* [witchtime] rewrite the key in graphics.ini (comments and other keys kept), take over the live keys; 0 = not written */
#endif
