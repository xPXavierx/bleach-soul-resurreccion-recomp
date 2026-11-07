#ifndef RSX_ASPECT_LAYOUT_H
#define RSX_ASPECT_LAYOUT_H

/* Ultrawide layouts of Bleach: Soul Resurreccion's menu screens (rsx_aspect.h,
 * ld_aspect_place in rsx_live_draw.c). A menu screen is dozens of 2D pieces --
 * tiled backgrounds, character art, frames, cards, particles, text -- that only
 * make sense in groups, so each screen is recognised by a texture only it draws
 * and laid out by its own table, made by comparing the game's own 16:9 frame
 * with the ultrawide one (claude-tools/ultrawide: jogar_telas.py, pecas.py):
 *  - a piece is placed by WHAT it is -- its texture (content hash, the same in
 *    every run), optionally narrowed to a region of the canvas and a minimum
 *    height when one texture holds several things -- never by where an
 *    animation happens to have it: elements sliding in must not jump;
 *  - background art is zoomed uniformly about the canvas centre until it
 *    covers the display (one transform for all its tiles: seamless, nothing
 *    distorted; a little is cropped above and below);
 *  - edge elements (titles, frames, button help, player box) go to their edge,
 *    keeping their distance to it;
 *  - everything else keeps the composition whole around the centre;
 *  - libfont's text has no texture of its own: a run of glyphs goes where its
 *    first glyph's zone says (left of x_left; right of x_right, right of
 *    x_right_low and below y_low, or right of x_right_top and above y_top).
 * A screen that is not in the table keeps its 16:9 layout around the centre
 * between black bars. */

typedef unsigned long long rsx_aspect_tex;

enum {
    RSX_LAYOUT_CENTRE,
    RSX_LAYOUT_COVER,     /* background: uniform zoom about the centre */
    RSX_LAYOUT_LEFT,
    RSX_LAYOUT_RIGHT
};

/* A texture's placement; the piece's box centre must lie in [x0,x1) x [y0,y1)
 * (x1 = 0: anywhere), its height be at least min_h and its width at most max_w
 * (0: any). First match wins. */
typedef struct { rsx_aspect_tex tex; float x0, y0, x1, y1, min_h; int placement; float max_w; } rsx_aspect_pin;

typedef struct {
    const char* name;
    rsx_aspect_tex detect[2];          /* drawn by this screen only */
    float x_left, x_right, x_right_low, y_low, x_right_top, y_top;   /* text zones */
    float cover_h;                     /* unpinned images at least this tall are background (0: none) */
    rsx_aspect_pin pins[24];
    /* 1: drawn over gameplay (pause...) -- unpinned pieces and text follow the
     * gameplay HUD rules (ld_aspect_place) instead of staying in the centre,
     * and the text zones above are unused (pin RSX_TEX_TEXT regions instead). */
    int hud;
    /* 1: character art over the background: unpinned images at least
     * RSX_COMPOSE_MIN_H tall (art tiles, not mode titles or the ring's
     * chevrons) zoom with the background, and with them every tile of their
     * texture; strips across the width still only stretch; 3D stays as it is. */
    int compose;
    /* 1: the screen's 3D is projected for 16:9 without the widened field of
     * view (the Collection's figure viewer): squeezed by 1/k to its shape. */
    int flat_3d;
    /* The detect textures count only in a piece at least this tall (0: any):
     * an atlas shared with other screens, recognised by its background. */
    float detect_h;
    /* Characters the screen draws twice, as a static 2D render and as the
     * animated 3D model over it, matched in place and scale (the game cuts
     * between them): 3D draws by these vertex programs (vp hash) that no pin
     * keeps as they are get `figure` -- the same placement as the 2D render
     * (RSX_LAYOUT_COVER: zoomed with the art; RSX_LAYOUT_RIGHT: at the right
     * edge with the portrait, and on into the level while the same model is
     * drawn, easing back as it slides to the centre). */
    unsigned figure_vp[2];
    int figure;
} rsx_aspect_screen;

#define RSX_COMPOSE_MIN_H 150.0f

/* Pins with this texture apply to libfont text runs (by their first glyph). */
#define RSX_TEX_TEXT 1ull

#define RSX_LAYOUT_NONE   -1e9f
#define RSX_LAYOUT_ALL     1e9f
#define RSX_ANY 0, 0, 0, 0, 0

/* Shared by the menus. */
#define RSX_TEX_SPARKLE_A   0x58958eedf23a9552ull   /* particles */
#define RSX_TEX_SPARKLE_B   0x21477842053a0f00ull
#define RSX_TEX_FRAME_LINES 0x5c550f8e2e5deea5ull   /* frame lines, also the ring's small chevrons */
#define RSX_TEX_RING        0x5bb9f18357209354ull   /* the mode ring around the cube */
#define RSX_TEX_SIDE_BG     0x0ddd0729b7ffaf5bull   /* background tile at the right edge */

/* Frame lines: the thin tall ones at both sides go to their edge, the
 * bottom-right one under the button help goes right, the top-left one under
 * the screen's title (sliding in from the left) goes left; the same texture also
 * draws the ring's chevrons and big background overlays, which stay put. */
#define RSX_PINS_FRAME \
    {RSX_TEX_FRAME_LINES, 0, -1e9f, 640, 1e9f, 300, RSX_LAYOUT_LEFT, 20}, \
    {RSX_TEX_FRAME_LINES, 640, -1e9f, 1e9f, 1e9f, 300, RSX_LAYOUT_RIGHT, 20}, \
    {RSX_TEX_FRAME_LINES, 900, 600, 1e9f, 1e9f, 0, RSX_LAYOUT_RIGHT, 0}, \
    {RSX_TEX_FRAME_LINES, -1e9f, 80, 640, 100, 0, RSX_LAYOUT_LEFT, 0}
#define RSX_PINS_PARTICLES \
    {RSX_TEX_SPARKLE_A, RSX_ANY, RSX_LAYOUT_COVER}, {RSX_TEX_SPARKLE_B, RSX_ANY, RSX_LAYOUT_COVER}
/* The combo counter left of the level -- "GOOD!!"/"NICE!!", its digits,
 * "SLASH!!", glow and underline -- (its textures also draw parts of the life
 * bar and the BEAT counter: only this region). */
#define RSX_PINS_COMBO \
    {0xb8155029d2e92c33ull, 0, 100, 640, 460, 0, RSX_LAYOUT_LEFT}, \
    {0x3045f6e091083897ull, 0, 100, 640, 460, 0, RSX_LAYOUT_LEFT}, \
    {0x9467e51bd82138b3ull, 0, 100, 640, 460, 0, RSX_LAYOUT_LEFT}, \
    {0x0defdc92e029c5d0ull, 0, 100, 640, 460, 0, RSX_LAYOUT_LEFT}, \
    {0x21866a71a02e53ccull, 0, 100, 640, 460, 0, RSX_LAYOUT_LEFT}
/* The life bar top left: frame, gauge, its caps and shine. Its right end lies
 * past the HUD's left band, so it goes by texture: the bar goes whole. */
#define RSX_PINS_LIFE \
    {0xee7b673cc760626aull, -1e9f, -1e9f, 900, 200, 0, RSX_LAYOUT_LEFT}, \
    {0x16d39a2aa5780da1ull, -1e9f, -1e9f, 900, 200, 0, RSX_LAYOUT_LEFT}, \
    {0x863b636281bb4552ull, -1e9f, -1e9f, 900, 200, 0, RSX_LAYOUT_LEFT}, \
    {0x5b3d7e79ffb9bb4bull, -1e9f, -1e9f, 900, 200, 0, RSX_LAYOUT_LEFT}, \
    {0x6e695ad4e7db75beull, -1e9f, -1e9f, 900, 200, 0, RSX_LAYOUT_LEFT}, \
    {0xf5a3f795c644834eull, -1e9f, -1e9f, 900, 200, 0, RSX_LAYOUT_LEFT}
/* Button help icons along the bottom right. */
#define RSX_PINS_HELP {0x62dce6620d000000ull, 600, 600, 1e9f, 1e9f, 0, RSX_LAYOUT_RIGHT}

static const rsx_aspect_screen rsx_aspect_screens[] = {
    /* "PUSH START button": flames and the full-screen Ichigo art behind; the
     * logo, copyright and prompt as one block right of centre, kept at the
     * same distance from the right edge. a85e2527 holds the logo (upper, at
     * most 700 wide), the speed streaks and the bottom band (background). The
     * colour art fading in is full height: background too. */
    { "titulo", {0x2955cac94af3d92cull, 0x22aa8bcaa91af473ull},
      RSX_LAYOUT_NONE, 660.0f, RSX_LAYOUT_ALL, RSX_LAYOUT_ALL, RSX_LAYOUT_ALL, RSX_LAYOUT_NONE, 700.0f,
      {{0x2955cac94af3d92cull, RSX_ANY, RSX_LAYOUT_COVER}, {RSX_TEX_SIDE_BG, RSX_ANY, RSX_LAYOUT_COVER},
       {0x22aa8bcaa91af473ull, RSX_ANY, RSX_LAYOUT_COVER},
       {0xa85e25276b95ff50ull, 0, -1e9f, 1e9f, 550, 0, RSX_LAYOUT_RIGHT, 700},
       {0xa85e25276b95ff50ull, RSX_ANY, RSX_LAYOUT_COVER},
       {0xe837705239000000ull, RSX_ANY, RSX_LAYOUT_RIGHT}, {0xf9a096497e000000ull, RSX_ANY, RSX_LAYOUT_RIGHT},
       RSX_PINS_PARTICLES} },
    /* Main menu (any mode): "MAIN MENU" and the mode's big vertical name (one
     * texture per mode) at the left edge; the characters' art zooms with the
     * background (compose) so it reaches both edges, and their animated 3D
     * models with it (figure: the game cuts between the two); ring, 3D cube
     * (its textures pinned) and mode title in the centre as they are; description strip across the width,
     * its text in the centre; dialogs (panel, icons) whole in the centre;
     * button help at the right; frame lines at their edges. */
    { "menu principal", {0xae36f22767ff3526ull, 0xb7d00bc8bc8f4acfull},
      RSX_LAYOUT_NONE, RSX_LAYOUT_ALL, 700.0f, 615.0f, RSX_LAYOUT_ALL, RSX_LAYOUT_NONE, 0,
      {{RSX_TEX_SIDE_BG, RSX_ANY, RSX_LAYOUT_COVER},
       {0xb7d00bc8bc8f4acfull, 0, -1e9f, 300, 100, 0, RSX_LAYOUT_LEFT},
       {0xae36f22767ff3526ull, 0, -1e9f, 300, 1e9f, 0, RSX_LAYOUT_LEFT},
       {0xf965dafa03944190ull, 0, -1e9f, 300, 1e9f, 0, RSX_LAYOUT_LEFT},
       {RSX_TEX_RING, RSX_ANY, RSX_LAYOUT_CENTRE},
       {0x25a7dff67a61360full, RSX_ANY, RSX_LAYOUT_CENTRE}, {0x597303bebf7fde03ull, RSX_ANY, RSX_LAYOUT_CENTRE},
       {0x01e1178c7a77fe09ull, RSX_ANY, RSX_LAYOUT_CENTRE}, {0x7ceb66cf00aee3fbull, RSX_ANY, RSX_LAYOUT_CENTRE},
       RSX_PINS_HELP, {0x62dce6620d000000ull, RSX_ANY, RSX_LAYOUT_CENTRE},
       {0x4df799d20bad6257ull, RSX_ANY, RSX_LAYOUT_CENTRE}, {0x58ebf4b195f8ee7cull, RSX_ANY, RSX_LAYOUT_CENTRE},
       RSX_PINS_FRAME, RSX_PINS_PARTICLES}, 0, 1, 0, 0, {0x6193C65Cu, 0xC01E9CB2u}, RSX_LAYOUT_COVER },
    /* Story (episode select): background tiles and particles cover; title top
     * left; PLAYER box, label and name top right; the cards, ring and cube stay
     * together in the centre; button help bottom right; frame lines. */
    { "story", {0x3c71f86ade254752ull, 0},
      RSX_LAYOUT_NONE, RSX_LAYOUT_ALL, 700.0f, 615.0f, 850.0f, 110.0f, 0,
      {{0x3c71f86ade254752ull, RSX_ANY, RSX_LAYOUT_COVER},
       {0xd51c905493000000ull, 0, -1e9f, 300, 110, 0, RSX_LAYOUT_LEFT},
       {0x7971018d49000000ull, RSX_ANY, RSX_LAYOUT_RIGHT},
       {0x3bc91f3a90000000ull, 800, -1e9f, 1e9f, 110, 0, RSX_LAYOUT_RIGHT},
       RSX_PINS_HELP, RSX_PINS_FRAME, RSX_PINS_PARTICLES} },
    /* Loading (move list, "Now Loading") and the episode introductions: the
     * flames and each episode's illustration (full-height layers) cover; the
     * character's black-and-white portrait at the right edge (by texture: its
     * tiles look like an illustration's), and the 3D cut-in of the same
     * character that fades in exactly over it and slides on into the level
     * with it (figure); move list, its icons and "Now Loading" at the left
     * edge; Begin/Skip at the right edge. */
    { "loading", {0xd66902c64e64ef6aull, 0x6f6ba28f13efe947ull},
      640.0f, 1060.0f, RSX_LAYOUT_ALL, RSX_LAYOUT_ALL, RSX_LAYOUT_ALL, RSX_LAYOUT_NONE, 700.0f,
      {{0xd66902c64e64ef6aull, RSX_ANY, RSX_LAYOUT_COVER}, {0x6f6ba28f13efe947ull, RSX_ANY, RSX_LAYOUT_COVER},
       {0xec7798cd1fd1d96bull, RSX_ANY, RSX_LAYOUT_RIGHT}, {0xe53dbf9361edcc5eull, RSX_ANY, RSX_LAYOUT_RIGHT},
       {0x5d8bbd81a8f3a3e6ull, RSX_ANY, RSX_LAYOUT_RIGHT},
       {0xa01b360e0f000000ull, RSX_ANY, RSX_LAYOUT_LEFT}, {0x0930f4aefa000000ull, RSX_ANY, RSX_LAYOUT_LEFT},
       {0x91b4367481000000ull, RSX_ANY, RSX_LAYOUT_LEFT}, {0x157b8e1bb4000000ull, RSX_ANY, RSX_LAYOUT_LEFT},
       {0x62dce6620d000000ull, 0, -1e9f, 640, 1e9f, 0, RSX_LAYOUT_LEFT},
       {0x62dce6620d000000ull, 1000, 580, 1e9f, 1e9f, 0, RSX_LAYOUT_RIGHT},
       RSX_PINS_PARTICLES}, 0, 0, 0, 0, {0x0F97E81Eu, 0x69E2B05Fu}, RSX_LAYOUT_RIGHT },
    /* Collection (figure select and viewer): background and its chains cover;
     * "COLLECTION" and its line top left; completion rate, and the viewer's
     * info panel (plate, labels, arrows, icons, text) top right; the name
     * plate in the centre; the description strip and the help band across
     * the width, the description at the left, the help (icons and text,
     * longer in the viewer) at the right. The figure is 3D without the
     * widened field of view (flat_3d). Recognised by its full-height
     * background: the atlas 3bc91f3a also draws Story's player box, and the
     * chains are Level Up's too. */
    { "colecao", {0x3bc91f3a90da6179ull, 0},
      300.0f, RSX_LAYOUT_ALL, 450.0f, 615.0f, 850.0f, 250.0f, 0,
      {{0x3bc91f3a90da6179ull, 0, 0, 0, 0, 600, RSX_LAYOUT_COVER},
       {0xafc5166f74e8d1c0ull, RSX_ANY, RSX_LAYOUT_COVER},
       {0x3bc91f3a90da6179ull, -1e9f, -1e9f, 300, 110, 0, RSX_LAYOUT_LEFT},
       {0x3bc91f3a90da6179ull, 800, -1e9f, 1e9f, 250, 0, RSX_LAYOUT_RIGHT},
       {0x7971018d4909245full, 800, -1e9f, 1e9f, 250, 0, RSX_LAYOUT_RIGHT},
       {0x62dce6620d000000ull, 800, -1e9f, 1e9f, 250, 0, RSX_LAYOUT_RIGHT},
       {0x62dce6620d000000ull, -1e9f, 600, 1e9f, 1e9f, 0, RSX_LAYOUT_RIGHT},
       RSX_PINS_FRAME, RSX_PINS_PARTICLES}, 0, 0, 1, 600.0f },
    /* Pause over gameplay: its window (frame, selection, clock, buttons, menu
     * text) and the big map with its markers stay together in the centre; the
     * level and the HUD behind it keep the gameplay layout (the minimap draws
     * from the big map's texture too, up in its corner). */
    { "pausa", {0x7bc8b8f19dc02af3ull, 0},   /* its labels' atlas: 4df799d2 is every dialog's panel */
      0, 0, 0, 0, 0, 0, 0,
      {{0x4df799d20bad6257ull, RSX_ANY, RSX_LAYOUT_CENTRE}, {0x7bc8b8f19dc02af3ull, RSX_ANY, RSX_LAYOUT_CENTRE},
       {0x58ebf4b195f8ee7cull, RSX_ANY, RSX_LAYOUT_CENTRE},
       {0xe106e13363e3c62bull, -1e9f, 100, 1e9f, 1e9f, 0, RSX_LAYOUT_CENTRE},
       {0xc60bd5fa08f42d5dull, 704, 104, 1216, 616, 0, RSX_LAYOUT_CENTRE},
       {0x62dce6620d000000ull, 150, 110, 620, 480, 0, RSX_LAYOUT_CENTRE},
       {RSX_TEX_TEXT, 150, 110, 620, 480, 0, RSX_LAYOUT_CENTRE}, RSX_PINS_COMBO, RSX_PINS_LIFE}, 1 },
    /* Gameplay (no screen drawn: always last, never detected). The combo
     * counter -- "GOOD!!"/"NICE!!", its digits, "SLASH!!", glow and underline,
     * left of the level -- goes left as one, with the life bar. */
    { "jogo", {0, 0},
      0, 0, 0, 0, 0, 0, 0,
      {RSX_PINS_COMBO, RSX_PINS_LIFE}, 1 },
};
#define RSX_SCREEN_GAMEPLAY (RSX_ASPECT_SCREENS - 1)
#define RSX_ASPECT_SCREENS ((int)(sizeof(rsx_aspect_screens) / sizeof(rsx_aspect_screens[0])))

/* Pins name a texture by its full hash, or by its upper 40 bits when the low
 * 24 are zero (the trace's short form). */
static inline int rsx_aspect_tex_is(rsx_aspect_tex pin, rsx_aspect_tex tex)
{
    return pin == tex || (!(pin & 0xFFFFFFull) && (tex & ~0xFFFFFFull) == pin);
}

/* The screen a texture identifies, drawn in a piece h tall, or -1. */
static inline int rsx_aspect_screen_of(rsx_aspect_tex tex, float h)
{
    if (!tex) return -1;
    for (int s = 0; s < RSX_ASPECT_SCREENS; s++)
        if ((rsx_aspect_screens[s].detect[0] == tex || rsx_aspect_screens[s].detect[1] == tex) &&
            h >= rsx_aspect_screens[s].detect_h) return s;
    return -1;
}

/* Whether screen s names this texture (detects or pins it). */
static inline int rsx_aspect_screen_has(int s, rsx_aspect_tex tex)
{
    const rsx_aspect_screen* screen = &rsx_aspect_screens[s];
    if (screen->detect[0] == tex || screen->detect[1] == tex) return 1;
    for (int i = 0; i < 24 && screen->pins[i].tex; i++)
        if (rsx_aspect_tex_is(screen->pins[i].tex, tex)) return 1;
    return 0;
}

/* The pinned placement of a piece of screen s, or -1. */
static inline int rsx_aspect_screen_pin(int s, rsx_aspect_tex tex, float x0, float y0, float x1, float y1)
{
    const rsx_aspect_screen* screen = &rsx_aspect_screens[s];
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    for (int i = 0; i < 24 && screen->pins[i].tex; i++) {
        const rsx_aspect_pin* p = &screen->pins[i];
        if (!rsx_aspect_tex_is(p->tex, tex) || y1 - y0 < p->min_h || (p->max_w > 0 && x1 - x0 > p->max_w)) continue;
        if (p->x1 != 0 && !(cx >= p->x0 && cx < p->x1 && cy >= p->y0 && cy < p->y1)) continue;
        return p->placement;
    }
    return -1;
}

/* Placement of a textured 2D piece of screen s from its box (strip: a thin
 * piece across the whole width, which only stretches unless pinned). */
static inline int rsx_aspect_screen_piece(int s, rsx_aspect_tex tex, float x0, float y0, float x1, float y1,
                                          int strip)
{
    const int pinned = rsx_aspect_screen_pin(s, tex, x0, y0, x1, y1);
    if (pinned >= 0) return pinned;
    if (strip) return RSX_LAYOUT_CENTRE;
    if (tex && rsx_aspect_screens[s].compose && y1 - y0 >= RSX_COMPOSE_MIN_H) return RSX_LAYOUT_COVER;
    if (rsx_aspect_screens[s].cover_h > 0 && y1 - y0 >= rsx_aspect_screens[s].cover_h) return RSX_LAYOUT_COVER;
    return RSX_LAYOUT_CENTRE;
}

/* Placement of a text run of screen s from its first glyph's centre. */
static inline int rsx_aspect_screen_text(int s, float cx, float cy)
{
    const rsx_aspect_screen* screen = &rsx_aspect_screens[s];
    if (cx < screen->x_left) return RSX_LAYOUT_LEFT;
    if (cx > screen->x_right || (cx > screen->x_right_low && cy > screen->y_low) ||
        (cx > screen->x_right_top && cy < screen->y_top)) return RSX_LAYOUT_RIGHT;
    return RSX_LAYOUT_CENTRE;
}

#endif
