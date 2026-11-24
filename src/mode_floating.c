// SPDX-License-Identifier: GPL-3.0-only

#include "config.h"
#include "log.h"
#include "mode.h"
#include "screencopy.h"
#include "state.h"
#include "target_detection.h"
#include "utils.h"
#include "utils_cairo.h"

#include <cairo.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define MIN_SUB_AREA_SIZE (25 * 50)

static void get_areas_from_stdin(struct floating_mode_state *ms) {
    size_t       areas_cap   = 256;
    struct rect *areas       = malloc(sizeof(struct rect) * areas_cap);
    int          areas_count = 0;
    char        *buf         = NULL;
    size_t       buf_n       = 0;

    while (getline(&buf, &buf_n, stdin) >= 0) {
        if (areas_count >= areas_cap) {
            areas_cap *= 2;
            areas      = realloc(areas, sizeof(struct rect) * areas_cap);
        }

        struct rect *curr_area     = &areas[areas_count];
        int          matched_count = sscanf(
            buf, "%dx%d+%d+%d", &curr_area->w, &curr_area->h, &curr_area->x,
            &curr_area->y
        );
        if (matched_count < 4) {
            LOG_ERR("Error parsing '%s'. Skipping.", buf);
            continue;
        }

        areas_count++;
    }

    free(buf);

    LOG_INFO("Got %d areas.", areas_count);

    ms->areas     = areas;
    ms->num_areas = areas_count;
}

#if OPENCV_ENABLED

static void get_area_from_screenshot(
    struct state *state, struct floating_mode_state *ms, struct rect area
) {
    // This is so that we don't capture window borders.
    area.x += 1;
    area.y += 1;
    area.h -= 2;
    area.w -= 2;

    struct scrcpy_buffer    *scrcpy_buffer = query_screenshot(state, area);
    enum wl_output_transform output_transform =
        state->current_output->transform;
    ms->num_areas = compute_target_from_img_buffer(
        scrcpy_buffer->data, scrcpy_buffer->height, scrcpy_buffer->width,
        scrcpy_buffer->stride, scrcpy_buffer->format, output_transform, area,
        &ms->areas
    );
    destroy_scrcpy_buffer(scrcpy_buffer);
}

#endif

void *floating_mode_enter(struct state *state, struct rect area) {
    struct floating_mode_state *ms = malloc(sizeof(*ms));

    ms->label_symbols =
        label_symbols_from_str(state->config.mode_floating.label_symbols);

    if (ms->label_symbols == NULL) {
        ms->areas           = NULL;
        ms->label_selection = NULL;
        state->running      = false;
        return ms;
    }

    switch (state->config.mode_floating.source) {
    case FLOATING_MODE_SOURCE_STDIN:
        get_areas_from_stdin(ms);
        break;
    case FLOATING_MODE_SOURCE_DETECT:
#if OPENCV_ENABLED
        get_area_from_screenshot(state, ms, area);
#else
        // This should not happen as the value is checked when loading the
        // configuration.
        LOG_ERR("Binary not built with OpenCV support.");
        exit(1);
#endif
        break;
    }

    ms->label_selection = label_selection_new(ms->label_symbols, ms->num_areas);

    ms->label_font_face = cairo_toy_font_face_create(
        state->config.mode_floating.label_font_family, CAIRO_FONT_SLANT_NORMAL,
        state->config.mode_floating.label_style == FLOATING_LABEL_STYLE_CHIP
            ? CAIRO_FONT_WEIGHT_BOLD
            : CAIRO_FONT_WEIGHT_NORMAL
    );

    return ms;
}

void floating_mode_reenter(struct state *state, void *mode_state) {
    struct floating_mode_state *ms = mode_state;
    label_selection_back(ms->label_selection);
}

static bool floating_mode_key(
    struct state *state, void *mode_state, xkb_keysym_t keysym, char *text
) {
    struct floating_mode_state *ms = mode_state;

    switch (keysym) {
    case XKB_KEY_BackSpace:
        return label_selection_back(ms->label_selection) != 0;

    case XKB_KEY_Escape:
        state->running = false;
        break;
    default:;
        int symbol_idx = label_symbols_find_idx(ms->label_symbols, text);
        if (symbol_idx < 0) {
            return false;
        }

        label_selection_append(ms->label_selection, symbol_idx);

        int idx = label_selection_to_idx(ms->label_selection);
        if (idx >= 0) {
            enter_next_mode(state, ms->areas[idx]);
        }
        return true;
    }

    return false;
}

// `label_uppercase`: labels are drawn in capitals; matching still uses the
// configured (lowercase) symbols, so keys are typed as before.
static void label_display_case(char *s, bool uppercase) {
    if (!uppercase) {
        return;
    }
    for (; *s; s++) {
        *s = toupper((unsigned char)*s);
    }
}

static void rounded_rect_path(
    cairo_t *cairo, double x, double y, double w, double h, double r
) {
    r = fmin(r, fmin(w, h) / 2);
    if (r <= 0) {
        cairo_rectangle(cairo, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cairo, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cairo, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cairo, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cairo);
}

struct chip_box {
    double x, y, w, h;
};

static bool chip_overlaps(struct chip_box c, struct chip_box *placed, int n) {
    for (int i = 0; i < n; i++) {
        struct chip_box p = placed[i];
        if (c.x < p.x + p.w && p.x < c.x + c.w && c.y < p.y + p.h &&
            p.y < c.y + c.h) {
            return true;
        }
    }
    return false;
}

// Where a region's chip goes: its left edge, vertically centered, unless that
// would cover a chip already drawn; then its right edge, just above it, or
// just below it, whichever is free first.
static struct chip_box
place_chip(struct rect a, double w, double h, struct chip_box *placed, int n) {
    const double    cy           = a.y + (a.h - h) / 2;
    struct chip_box candidates[] = {
        {a.x, cy, w, h},
        {a.x + a.w - w, cy, w, h},
        {a.x, a.y - h, w, h},
        {a.x, a.y + a.h, w, h},
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
        if (!chip_overlaps(candidates[i], placed, n)) {
            return candidates[i];
        }
    }
    return candidates[0];
}

// `label_style=chip`: a small rounded label at the left edge of each region,
// vertically centered on it, moved aside when it would cover another chip.
// Chips that no longer match the typed keys are drawn with `chip_dim_color`
// (or hidden when it is fully transparent).
static void render_chips(
    struct floating_mode_state *ms, struct mode_floating_config *config,
    cairo_t *cairo
) {
    label_selection_t *curr_label =
        label_selection_new(ms->label_symbols, ms->num_areas);
    label_selection_set_from_idx(curr_label, 0);

    int  label_str_max_len = label_selection_str_max_len(curr_label) + 1;
    char label_selected_str[label_str_max_len];
    char label_unselected_str[label_str_max_len];

    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_u32(cairo, config->unselectable_bg_color);
    cairo_paint(cairo);

    cairo_set_font_face(cairo, ms->label_font_face);
    cairo_set_font_size(cairo, config->chip_font_size);
    cairo_font_extents_t fe;
    cairo_font_extents(cairo, &fe);
    const double     pad        = config->chip_padding;
    const double     chip_h     = fe.ascent + fe.descent + 2 * pad;
    const bool       show_dim   = (config->chip_dim_color & 0xff) != 0;
    struct chip_box *placed     = malloc(sizeof(*placed) * (ms->num_areas + 1));
    int              num_placed = 0;

    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    for (int i = 0; i < ms->num_areas; i++, label_selection_incr(curr_label)) {
        const bool selectable =
            label_selection_is_included(curr_label, ms->label_selection);
        if (!selectable && !show_dim) {
            continue;
        }

        struct rect a = ms->areas[i];
        label_selection_str(curr_label, label_selected_str);
        label_display_case(label_selected_str, config->label_uppercase);
        cairo_text_extents_t te_all;
        cairo_text_extents(cairo, label_selected_str, &te_all);
        const double    chip_w = fmax(te_all.x_advance + 2 * pad, chip_h);
        struct chip_box box = place_chip(a, chip_w, chip_h, placed, num_placed);
        placed[num_placed++] = box;
        const double x = box.x, y = box.y;

        rounded_rect_path(cairo, x, y, chip_w, chip_h, config->chip_radius);
        cairo_set_source_u32(cairo, config->chip_bg_color);
        if (selectable) {
            cairo_fill_preserve(cairo);
            cairo_set_source_u32(cairo, config->chip_border_color);
            cairo_set_line_width(cairo, 1);
            cairo_stroke(cairo);
        } else {
            cairo_fill(cairo);
        }

        cairo_move_to(
            cairo, x + (chip_w - te_all.x_advance) / 2, y + pad + fe.ascent
        );
        if (selectable) {
            label_selection_str_split(
                curr_label, label_selected_str, label_unselected_str,
                ms->label_selection->next
            );
            label_display_case(label_selected_str, config->label_uppercase);
            label_display_case(label_unselected_str, config->label_uppercase);
            cairo_set_source_u32(cairo, config->label_select_color);
            cairo_show_text(cairo, label_selected_str);
            cairo_set_source_u32(cairo, config->label_color);
            cairo_show_text(cairo, label_unselected_str);
        } else {
            cairo_set_source_u32(cairo, config->chip_dim_color);
            cairo_show_text(cairo, label_selected_str);
        }
    }

    free(placed);
    label_selection_free(curr_label);
}

void floating_mode_render(
    struct state *state, void *mode_state, cairo_t *cairo
) {
    struct floating_mode_state  *ms     = mode_state;
    struct mode_floating_config *config = &state->config.mode_floating;

    if (config->label_style == FLOATING_LABEL_STYLE_CHIP) {
        render_chips(ms, config, cairo);
        return;
    }

    label_selection_t *curr_label =
        label_selection_new(ms->label_symbols, ms->num_areas);
    label_selection_set_from_idx(curr_label, 0);

    int  label_str_max_len = label_selection_str_max_len(curr_label) + 1;
    char label_selected_str[label_str_max_len];
    char label_unselected_str[label_str_max_len];

    cairo_set_font_face(cairo, ms->label_font_face);

    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_u32(cairo, config->unselectable_bg_color);
    cairo_paint(cairo);

    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    for (int i = 0; i < ms->num_areas; i++) {
        const bool selectable =
            label_selection_is_included(curr_label, ms->label_selection);

        if (selectable) {
            struct rect a = ms->areas[i];
            cairo_set_source_rgba(cairo, 0, 0, 0, 0);
            cairo_rectangle(cairo, a.x, a.y, a.w, a.h);
            cairo_fill(cairo);
        }

        label_selection_incr(curr_label);
    }

    label_selection_set_from_idx(curr_label, 0);
    for (int i = 0; i < ms->num_areas; i++) {
        struct rect a = ms->areas[i];

        const bool selectable =
            label_selection_is_included(curr_label, ms->label_selection);

        if (selectable) {
            cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
            cairo_set_source_u32(cairo, config->selectable_bg_color);
            cairo_rectangle(cairo, a.x, a.y, a.w, a.h);
            cairo_fill(cairo);

            cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_u32(cairo, config->selectable_border_color);
            cairo_rectangle(cairo, a.x + .5, a.y + .5, a.w - 1, a.h - 1);
            cairo_set_line_width(cairo, 1);
            cairo_stroke(cairo);

            cairo_set_font_size(
                cairo, compute_relative_font_size(&config->label_font_size, a.h)
            );
            cairo_text_extents_t te_all;
            label_selection_str(curr_label, label_selected_str);
            label_display_case(label_selected_str, config->label_uppercase);
            cairo_text_extents(cairo, label_selected_str, &te_all);

            label_selection_str_split(
                curr_label, label_selected_str, label_unselected_str,
                ms->label_selection->next
            );
            label_display_case(label_selected_str, config->label_uppercase);
            label_display_case(label_unselected_str, config->label_uppercase);

            cairo_text_extents_t te_selected, te_unselected;
            cairo_text_extents(cairo, label_selected_str, &te_selected);
            cairo_text_extents(cairo, label_unselected_str, &te_unselected);

            cairo_move_to(
                cairo,
                a.x +
                    (a.w - te_selected.x_advance - te_unselected.x_advance) / 2,
                a.y + (int)((a.h + te_all.height) / 2)
            );
            cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
            cairo_set_source_u32(cairo, config->label_select_color);
            cairo_show_text(cairo, label_selected_str);
            cairo_set_source_u32(cairo, config->label_color);
            cairo_show_text(cairo, label_unselected_str);
        }

        label_selection_incr(curr_label);
    }

    label_selection_free(curr_label);
}

void floating_mode_free(void *mode_state) {
    struct floating_mode_state *ms = mode_state;
    free(ms->areas);
    cairo_font_face_destroy(ms->label_font_face);
    label_selection_free(ms->label_selection);
    label_symbols_free(ms->label_symbols);
    free(ms);
}

static void floating_mode_restart(struct state *state, void *mode_state) {
    struct floating_mode_state *ms = mode_state;
    label_selection_clear(ms->label_selection);
}

struct mode_interface floating_mode_interface = {
    .name    = "floating",
    .enter   = floating_mode_enter,
    .reenter = floating_mode_reenter,
    .key     = floating_mode_key,
    .render  = floating_mode_render,
    .free    = floating_mode_free,
    .restart = floating_mode_restart,
};
