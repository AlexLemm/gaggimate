#include "Annotator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <utility>

#include <display/main.h>
#include <display/ui/default/eez/screens.h>
#include <lvgl.h>

namespace {

constexpr const char *DIR_ROOT = "annotations";
constexpr const char *DIR_SHOTS = "annotations/shots";
constexpr const char *NOTES_FILE = "annotations/ANNOTATIONS.md";

struct Pending {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<uint8_t> frame;
    std::string screen, mode, profile, nearby;
    float temp = 0, targetTemp = 0, pressure = 0;
};

Pending s_pending;
lv_obj_t *s_panel = nullptr;
lv_obj_t *s_textarea = nullptr;
lv_obj_t *s_marker = nullptr;

// --- UI context ---------------------------------------------------------------------------

const char *screenName(lv_obj_t *scr) {
    if (scr == objects.standby_screen)
        return "Standby screen";
    if (scr == objects.brew_screen)
        return "Brew screen";
    if (scr == objects.status_screen)
        return "Status screen";
    if (scr == objects.menu_screen_new)
        return "Menu screen";
    if (scr == objects.steam_screen)
        return "Steam screen";
    if (scr == objects.water_screen)
        return "Water screen";
    if (scr == objects.grind_screen)
        return "Grind screen";
    if (scr == objects.info_screen)
        return "Info screen";
    if (scr == objects.new_profile_screen)
        return "New profile screen";
    return "Unknown screen";
}

const char *modeName(int mode) {
    switch (mode) {
    case MODE_STANDBY:
        return "Standby";
    case MODE_BREW:
        return "Brew";
    case MODE_STEAM:
        return "Steam";
    case MODE_WATER:
        return "Water";
    case MODE_GRIND:
        return "Grind";
    default:
        return "Unknown";
    }
}

void collectLabels(lv_obj_t *obj, int x, int y, std::vector<std::pair<long, std::string>> &out) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
        return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        if (text && text[0]) {
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            const long dx = x < a.x1 ? a.x1 - x : (x > a.x2 ? x - a.x2 : 0);
            const long dy = y < a.y1 ? a.y1 - y : (y > a.y2 ? y - a.y2 : 0);
            out.emplace_back(dx * dx + dy * dy, text);
        }
    }
    const uint32_t count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < count; i++)
        collectLabels(lv_obj_get_child(obj, i), x, y, out);
}

// The closest on-screen text is what identifies a widget: EEZ object names do not exist at runtime.
std::string nearbyText(int x, int y) {
    std::vector<std::pair<long, std::string>> labels;
    collectLabels(lv_scr_act(), x, y, labels);
    std::stable_sort(labels.begin(), labels.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::string result;
    int used = 0;
    for (const auto &label : labels) {
        if (result.find("\"" + label.second + "\"") != std::string::npos)
            continue;
        if (used++ == 3)
            break;
        if (!result.empty())
            result += ", ";
        result += "\"" + label.second + "\"";
    }
    return result.empty() ? "(none)" : result;
}

// --- PNG output ---------------------------------------------------------------------------

uint32_t crcUpdate(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

void putBE32(std::vector<uint8_t> &v, uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8)
        v.push_back(static_cast<uint8_t>(n >> shift));
}

void writeChunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> head;
    putBE32(head, static_cast<uint32_t>(data.size()));
    fwrite(head.data(), 1, 4, f);
    fwrite(type, 1, 4, f);
    if (!data.empty())
        fwrite(data.data(), 1, data.size(), f);
    uint32_t crc = crcUpdate(0xFFFFFFFFu, reinterpret_cast<const uint8_t *>(type), 4);
    crc = crcUpdate(crc, data.data(), data.size()) ^ 0xFFFFFFFFu;
    std::vector<uint8_t> tail;
    putBE32(tail, crc);
    fwrite(tail.data(), 1, 4, f);
}

// Uncompressed ("stored" deflate) PNG: needs no image library, and annotations stay local.
bool writePng(const std::string &path, const std::vector<uint8_t> &rgb, int w, int h) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(signature, 1, 8, f);

    std::vector<uint8_t> ihdr;
    putBE32(ihdr, static_cast<uint32_t>(w));
    putBE32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    writeChunk(f, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(w) * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0); // no row filter
        const auto row = rgb.begin() + static_cast<long>(y) * w * 3;
        raw.insert(raw.end(), row, row + w * 3);
    }

    std::vector<uint8_t> idat = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size();) {
        const size_t len = std::min<size_t>(raw.size() - pos, 65535);
        idat.push_back(pos + len == raw.size() ? 1 : 0);
        idat.push_back(len & 0xFF);
        idat.push_back(len >> 8);
        idat.push_back(~len & 0xFF);
        idat.push_back((~len >> 8) & 0xFF);
        idat.insert(idat.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + len));
        pos += len;
    }
    putBE32(idat, (b << 16) | a);
    writeChunk(f, "IDAT", idat);
    writeChunk(f, "IEND", {});
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

void drawMarker(std::vector<uint8_t> &rgb, int w, int h, int cx, int cy) {
    constexpr int outer = 16, inner = 12;
    for (int y = std::max(0, cy - outer); y <= std::min(h - 1, cy + outer); y++) {
        for (int x = std::max(0, cx - outer); x <= std::min(w - 1, cx + outer); x++) {
            const int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d <= outer * outer && d >= inner * inner) {
                uint8_t *p = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                p[0] = 0xFF;
                p[1] = 0x30;
                p[2] = 0x30;
            }
        }
    }
}

// --- Saving -------------------------------------------------------------------------------

bool save(const std::string &note) {
    mkdir(DIR_ROOT, 0755);
    mkdir(DIR_SHOTS, 0755);

    const std::time_t now = std::time(nullptr);
    // UTC: the firmware sets its own TZ, so local time would disagree with web UI notes.
    std::tm utc{};
    gmtime_r(&now, &utc);
    char stamp[32], when[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &utc);

    const std::string shotName = std::string("touch-") + stamp + ".png";
    drawMarker(s_pending.frame, s_pending.w, s_pending.h, s_pending.x, s_pending.y);
    const bool shotOk = writePng(std::string(DIR_SHOTS) + "/" + shotName, s_pending.frame, s_pending.w, s_pending.h);

    struct stat st {};
    const bool isNew = stat(NOTES_FILE, &st) != 0;
    FILE *f = fopen(NOTES_FILE, "a");
    if (!f)
        return false;
    if (isNew)
        fputs("# GaggiMate design notes\n\nWritten by right-clicking the simulator or the web UI dev server.\n", f);
    fprintf(f, "\n## %s - Touchscreen - %s\n\n", when, s_pending.screen.c_str());
    fprintf(f, "- **Where:** x=%d, y=%d on the 480x480 panel (red ring in the screenshot)\n", s_pending.x, s_pending.y);
    fprintf(f, "- **Nearby text:** %s\n", s_pending.nearby.c_str());
    fprintf(f, "- **Machine state:** mode %s, %.1f C (target %.1f C), %.1f bar, profile \"%s\"\n", s_pending.mode.c_str(),
            s_pending.temp, s_pending.targetTemp, s_pending.pressure, s_pending.profile.c_str());
    fprintf(f, "- **Firmware:** %s\n", BUILD_GIT_VERSION);
    if (shotOk)
        fprintf(f, "- **Screenshot:** shots/%s\n", shotName.c_str());
    fprintf(f, "\n**Note:** %s\n", note.c_str());
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

// --- Dialog -------------------------------------------------------------------------------

void showToast(const char *text) {
    lv_obj_t *toast = lv_label_create(lv_layer_top());
    lv_label_set_text(toast, text);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x1E7D3C), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(toast, lv_color_white(), 0);
    lv_obj_set_style_pad_all(toast, 8, 0);
    lv_obj_set_style_radius(toast, 6, 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_del_delayed(toast, 1500);
}

void closeDialog() {
    lv_obj_del(s_panel);
    lv_obj_del(s_marker);
    s_panel = s_textarea = s_marker = nullptr;
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_invalidate(lv_scr_act()); // a partial redraw leaves seams across the logo image
    SDL_StopTextInput();
    s_pending = Pending{};
}

} // namespace

namespace Annotator {

bool isOpen() { return s_panel != nullptr; }

void open(int x, int y, std::vector<uint8_t> frameRGB24, int width, int height) {
    if (isOpen())
        return;

    s_pending.x = x;
    s_pending.y = y;
    s_pending.w = width;
    s_pending.h = height;
    s_pending.frame = std::move(frameRGB24);
    s_pending.screen = screenName(lv_scr_act());
    s_pending.nearby = nearbyText(x, y);
    s_pending.mode = modeName(controller.getMode());
    s_pending.temp = controller.getCurrentTemp();
    s_pending.targetTemp = controller.getTargetTemp();
    s_pending.pressure = controller.getCurrentPressure();
    s_pending.profile = controller.getProfileManager()->getSelectedProfile().label.c_str();

    // Swallow clicks so the firmware UI underneath stays exactly as it was annotated.
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);

    s_marker = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_marker);
    lv_obj_set_size(s_marker, 32, 32);
    lv_obj_set_pos(s_marker, x - 16, y - 16);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_marker, 4, 0);
    lv_obj_set_style_border_color(s_marker, lv_color_hex(0xFF3030), 0);

    // Sized to stay inside the round panel, in whichever half keeps the marked spot visible.
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, 340, 190);
    lv_obj_set_pos(s_panel, 70, y > 240 ? 75 : 215);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0xFF3030), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_pad_all(s_panel, 10, 0);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text_fmt(title, "Note: %s", s_pending.screen.c_str());
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_textarea = lv_textarea_create(s_panel);
    lv_obj_set_size(s_textarea, 316, 104);
    lv_obj_align(s_textarea, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_textarea_set_placeholder_text(s_textarea, "What should this do, and how?");
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(0x101010), 0);
    lv_obj_set_style_text_color(s_textarea, lv_color_white(), 0);
    lv_obj_add_state(s_textarea, LV_STATE_FOCUSED);

    lv_obj_t *hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "Enter save, Shift+Enter line, Esc cancel");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(hint, 316);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    SDL_StartTextInput();
}

bool handleEvent(const SDL_Event &e) {
    if (!isOpen())
        return false;

    switch (e.type) {
    case SDL_TEXTINPUT:
        lv_textarea_add_text(s_textarea, e.text.text);
        return true;
    case SDL_KEYUP:
        return true;
    case SDL_KEYDOWN:
        break;
    default:
        return false;
    }

    const SDL_Keycode key = e.key.keysym.sym;
    const SDL_Keymod mods = static_cast<SDL_Keymod>(e.key.keysym.mod);
    if (key == SDLK_ESCAPE) {
        closeDialog();
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (mods & KMOD_SHIFT) {
            lv_textarea_add_char(s_textarea, '\n');
        } else {
            const std::string note = lv_textarea_get_text(s_textarea);
            if (note.empty()) {
                closeDialog(); // nothing written, so there is nothing to keep
            } else {
                const bool ok = save(note);
                closeDialog();
                showToast(ok ? "Note saved" : "Could not save note");
            }
        }
    } else if (key == SDLK_BACKSPACE) {
        lv_textarea_del_char(s_textarea);
    } else if (key == SDLK_DELETE) {
        lv_textarea_del_char_forward(s_textarea);
    } else if (key == SDLK_LEFT) {
        lv_textarea_cursor_left(s_textarea);
    } else if (key == SDLK_RIGHT) {
        lv_textarea_cursor_right(s_textarea);
    } else if (key == SDLK_UP) {
        lv_textarea_cursor_up(s_textarea);
    } else if (key == SDLK_DOWN) {
        lv_textarea_cursor_down(s_textarea);
    } else if (key == SDLK_v && (mods & KMOD_CTRL)) {
        if (char *clip = SDL_GetClipboardText()) {
            lv_textarea_add_text(s_textarea, clip);
            SDL_free(clip);
        }
    }
    return true;
}

} // namespace Annotator
