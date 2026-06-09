/*
 * Teams Mute - a one-button BLE HID remote for Flipper Zero.
 *
 * Pairs to a PC as a Bluetooth keyboard and, on OK, sends Win+Alt+K - the
 * Windows 11 system-wide taskbar microphone toggle - so Teams (or any app)
 * can be muted/unmuted from across the room regardless of the focused window.
 *
 * The BLE lifecycle (swap to the HID profile on entry, restore the default
 * profile on exit, keep bonds in an app-private key file) mirrors the stock
 * "Bluetooth Remote" / air_mouse apps in flipperzero-good-faps.
 */

#include <furi.h>
#include <stdio.h>

#include <furi_hal_bt.h>
#include <furi_hal_usb_hid.h> // KEY_MOD_* modifier bits + HID_KEYBOARD_* usage codes

#include <storage/storage.h> // APP_DATA_PATH()
#include <gui/gui.h>
#include <input/input.h>

#include <bt/bt_service/bt.h>
#include "teams_mute_hid_profile.h" // vendored BLE HID profile (see file header)

#include <teams_mute_icons.h>

#define TAG "TeamsMute"

// BLE bonding keys live in an app-private file so pairing this remote never
// overwrites the Flipper's own system Bluetooth keys.
#define TEAMS_MUTE_KEYS_PATH APP_DATA_PATH("teams_mute.keys")

// Advertised BLE device-name prefix. hid_profile.h requires fewer than 8 chars.
#define TEAMS_MUTE_NAME_PREFIX "Mute"

// A Flipper HID keycode packs the modifier bits (high byte, KEY_MOD_* from
// furi_hal_usb_hid.h) together with the HID usage code (low byte).
#define KEY_TEAMS_TOGGLE (HID_KEYBOARD_K | KEY_MOD_LEFT_GUI | KEY_MOD_LEFT_ALT) // Win+Alt+K
#define KEY_TEAMS_MUTE   (HID_KEYBOARD_M | KEY_MOD_LEFT_CTRL | KEY_MOD_LEFT_SHIFT) // Ctrl+Shift+M
#define KEY_PUSH_TO_TALK (HID_KEYBOARD_SPACEBAR | KEY_MOD_LEFT_CTRL) // Ctrl+Space

// How long a "tap" is held down before release - long enough for the host to
// latch the system hotkey, short enough to feel instant.
#define KEY_TAP_HOLD_MS 40

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;

    Bt* bt;
    FuriHalBleProfileBase* ble_hid_profile;

    // Shared with the draw and BT-status callbacks; guard every access with mutex.
    BtStatus bt_status;
    uint32_t toggle_count;
    bool ptt_active;
} TeamsMute;

static const BleProfileHidParams ble_hid_params = {
    .device_name_prefix = TEAMS_MUTE_NAME_PREFIX,
    .mac_xor = 0x0001,
};

static const char* teams_mute_status_text(BtStatus status) {
    switch(status) {
    case BtStatusConnected:
        return "Connected";
    case BtStatusAdvertising:
        return "Advertising";
    case BtStatusOff:
        return "Off";
    default:
        return "Unavailable";
    }
}

/* ---------------------------------------------------------------- UI ----- */

static void teams_mute_draw_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    TeamsMute* app = ctx;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    BtStatus status = app->bt_status;
    uint32_t count = app->toggle_count;
    bool ptt = app->ptt_active;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Teams Mute");

    char counter[16];
    snprintf(counter, sizeof(counter), "Sent:%lu", (unsigned long)count);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 126, 11, AlignRight, AlignBottom, counter);

    canvas_draw_line(canvas, 0, 15, 127, 15);

    // Connection / pairing status.
    canvas_draw_str(canvas, 2, 27, "BT:");
    canvas_draw_str(canvas, 22, 27, teams_mute_status_text(status));

    if(ptt) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 44, "PUSH TO TALK");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 58, "Release OK to stop");
    } else {
        canvas_draw_str(canvas, 2, 39, "OK: Mute  (Win+Alt+K)");
        canvas_draw_str(canvas, 2, 49, "Up: Ctrl+Shift+M");
        canvas_draw_str(canvas, 2, 59, "Hold OK: Talk (PTT)");
    }
}

static void teams_mute_input_callback(InputEvent* event, void* ctx) {
    furi_assert(ctx);
    TeamsMute* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

static void teams_mute_bt_status_callback(BtStatus status, void* ctx) {
    furi_assert(ctx);
    TeamsMute* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->bt_status = status;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

/* ----------------------------------------------------------- HID keys ---- */

static bool teams_mute_is_connected(TeamsMute* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool connected = (app->bt_status == BtStatusConnected);
    furi_mutex_release(app->mutex);
    return connected;
}

// Press, hold briefly, then release a packed modifier+key combo.
static void teams_mute_tap(TeamsMute* app, uint16_t keycode) {
    ble_profile_hid_kb_press(app->ble_hid_profile, keycode);
    furi_delay_ms(KEY_TAP_HOLD_MS);
    ble_profile_hid_kb_release(app->ble_hid_profile, keycode);
}

static void teams_mute_count_inc(TeamsMute* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->toggle_count++;
    furi_mutex_release(app->mutex);
}

static void teams_mute_handle_input(TeamsMute* app, const InputEvent* event) {
    const bool connected = teams_mute_is_connected(app);

    if(event->key == InputKeyOk) {
        if(event->type == InputTypeShort) {
            // Tap OK: toggle the Windows system mic mute (Win+Alt+K).
            if(connected) {
                teams_mute_tap(app, KEY_TEAMS_TOGGLE);
                teams_mute_count_inc(app);
            }
        } else if(event->type == InputTypeLong) {
            // Hold OK: push-to-talk - press Ctrl+Space and keep it held down.
            if(connected) {
                ble_profile_hid_kb_press(app->ble_hid_profile, KEY_PUSH_TO_TALK);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->ptt_active = true;
                furi_mutex_release(app->mutex);
            }
        } else if(event->type == InputTypeRelease) {
            // Releasing OK ends push-to-talk (release Ctrl+Space).
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            const bool was_ptt = app->ptt_active;
            app->ptt_active = false;
            furi_mutex_release(app->mutex);
            if(was_ptt) {
                ble_profile_hid_kb_release(app->ble_hid_profile, KEY_PUSH_TO_TALK);
            }
        }
    } else if(event->key == InputKeyUp && event->type == InputTypeShort) {
        // Up: app-level Teams mute shortcut fallback (Ctrl+Shift+M).
        if(connected) {
            teams_mute_tap(app, KEY_TEAMS_MUTE);
            teams_mute_count_inc(app);
        }
    }
}

/* ---------------------------------------------------------- BLE setup ---- */

static void teams_mute_bt_start(TeamsMute* app) {
    app->bt = furi_record_open(RECORD_BT);
    bt_disconnect(app->bt);
    furi_delay_ms(200); // let the 2nd core settle after disconnect
    bt_keys_storage_set_storage_path(app->bt, TEAMS_MUTE_KEYS_PATH);
    app->ble_hid_profile = bt_profile_start(app->bt, ble_profile_hid, (void*)&ble_hid_params);
    furi_check(app->ble_hid_profile);
    furi_hal_bt_start_advertising();
    bt_set_status_changed_callback(app->bt, teams_mute_bt_status_callback, app);
}

static void teams_mute_bt_stop(TeamsMute* app) {
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    ble_profile_hid_kb_release_all(app->ble_hid_profile); // drop any key still held
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(app->bt);
    furi_check(bt_profile_restore_default(app->bt));
    furi_record_close(RECORD_BT);
    app->bt = NULL;
}

/* -------------------------------------------------------- app alloc ------ */

static TeamsMute* teams_mute_alloc(void) {
    TeamsMute* app = malloc(sizeof(TeamsMute));

    app->bt = NULL;
    app->ble_hid_profile = NULL;
    app->bt_status = BtStatusUnavailable;
    app->toggle_count = 0;
    app->ptt_active = false;

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, teams_mute_draw_callback, app);
    view_port_input_callback_set(app->view_port, teams_mute_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void teams_mute_free(TeamsMute* app) {
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    app->gui = NULL;

    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);
}

int32_t teams_mute_app(void* p) {
    UNUSED(p);

    TeamsMute* app = teams_mute_alloc();
    teams_mute_bt_start(app);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(event.key == InputKeyBack && event.type == InputTypeShort) {
            running = false;
        } else {
            teams_mute_handle_input(app, &event);
        }

        view_port_update(app->view_port);
    }

    teams_mute_bt_stop(app);
    teams_mute_free(app);
    return 0;
}
