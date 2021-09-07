// Build with: gcc -o main main.c `pkg-config --libs --cflags mpv sdl2` -std=c99

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL2/SDL.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#ifdef __SWITCH__
#   include <switch.h>
#endif

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

#ifdef __SWITCH__
void userAppInit(void) {
    socketInitializeDefault();
    nxlinkStdio();
}

void userAppExit() {
    socketExit();
}
#endif

int main(int argc, char **argv) {
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("context init failed");

    // Some minor options can only be set before mpv_initialize().
    if (mpv_initialize(mpv) < 0)
        die("mpv init failed");

    mpv_request_log_messages(mpv, "v");

#ifdef __SWITCH__
    mpv_set_option_string(mpv, "ao", "hos");
    mpv_set_option_string(mpv, "vo", "hos");
#endif

    mpv_set_option_string(mpv, "hwdec", "auto");
    mpv_set_option_string(mpv, "hwdec-codecs", "h264,mpeg1video,mpeg2video");

    const char *cmd[] = { "loadfile", argv[1], NULL };
    if (mpv_command(mpv, cmd) < 0)
        die("failed to load file");

    // Let it play, and wait until the user quits.
    while (1) {
        mpv_event *event = mpv_wait_event(mpv, 10000);
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *msg = event->data;
            printf("[%s]: %s", msg->prefix, msg->text);
        } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
            break;
        } else if (event->event_id != MPV_EVENT_NONE) {
            printf("event: %s\n", mpv_event_name(event->event_id));
        }
    }

    mpv_terminate_destroy(mpv);

    printf("properly terminated\n");
    return 0;
}
