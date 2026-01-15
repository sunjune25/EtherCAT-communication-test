// gpio_toggle_flag.c
// Build: gcc -O2 -Wall -o gpio_toggle_flag gpio_toggle_flag.c -lgpiod -lpthread
// Run  : sudo ./gpio_toggle_flag
//
// Input : /dev/gpiochip0 offset 106 (falling edge -> "callback" sets toggle_flag=1)
// Output: /dev/gpiochip0 offset 85  (main loop toggles when toggle_flag==1)

#include <gpiod.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000LL);
}

struct isr_like_ctx {
    struct gpiod_line *in_line;
    atomic_int running;
    atomic_int toggle_flag; // 0/1

    long long debounce_us;
    long long last_us;
};

// falling edge 발생 시 "interrupt callback" 역할: flag만 ON
static void* isr_like_callback_thread(void *arg)
{
    struct isr_like_ctx *c = (struct isr_like_ctx*)arg;

    while (atomic_load(&c->running)) {
        // 너무 오래 block되면 종료가 불편하니 timeout 주고 루프
        struct timespec to;
        to.tv_sec = 0;
        to.tv_nsec = 200 * 1000 * 1000; // 200ms

        int ret = gpiod_line_event_wait(c->in_line, &to);
        if (ret < 0) {
            fprintf(stderr, "gpiod_line_event_wait error: %s\n", strerror(errno));
            break;
        }
        if (ret == 0) continue; // timeout

        struct gpiod_line_event ev;
        if (gpiod_line_event_read(c->in_line, &ev) < 0) {
            fprintf(stderr, "gpiod_line_event_read error: %s\n", strerror(errno));
            break;
        }

        if (ev.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
            long long t = now_us();
            if (t - c->last_us < c->debounce_us) continue;
            c->last_us = t;

            // ISR에서 하는 일: 플래그 ON (이미 1이어도 그냥 1 유지)
            atomic_store(&c->toggle_flag, 1);
        }
    }

    atomic_store(&c->running, 0);
    return NULL;
}

int main(void)
{
    const char *chip_path = "/dev/gpiochip0";
    const unsigned int in_offset  = 106; // input
    const unsigned int out_offset = 43;  // output

    struct gpiod_chip *chip = gpiod_chip_open(chip_path);
    if (!chip) {
        fprintf(stderr, "gpiod_chip_open(%s) failed: %s\n", chip_path, strerror(errno));
        return 1;
    }

    struct gpiod_line *in_line = gpiod_chip_get_line(chip, in_offset);
    if (!in_line) {
        fprintf(stderr, "gpiod_chip_get_line(in_offset=%u) failed: %s\n", in_offset, strerror(errno));
        gpiod_chip_close(chip);
        return 1;
    }

    struct gpiod_line *out_line = gpiod_chip_get_line(chip, out_offset);
    if (!out_line) {
        fprintf(stderr, "gpiod_chip_get_line(out_offset=%u) failed: %s\n", out_offset, strerror(errno));
        gpiod_chip_close(chip);
        return 1;
    }

    // output 요청 (초기 LOW)
    int out_state = 0;
    if (gpiod_line_request_output(out_line, "out85-toggle", out_state) < 0) {
        fprintf(stderr, "gpiod_line_request_output(offset=%u) failed: %s\n", out_offset, strerror(errno));
        gpiod_chip_close(chip);
        return 1;
    }

    // input falling edge 이벤트 요청
    if (gpiod_line_request_falling_edge_events(in_line, "in106-falling") < 0) {
        fprintf(stderr, "gpiod_line_request_falling_edge_events(offset=%u) failed: %s\n", in_offset, strerror(errno));
        gpiod_line_release(out_line);
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Input  (falling edge): %s offset %u\n", chip_path, in_offset);
    printf("Output (toggle)      : %s offset %u (initial=%d)\n", chip_path, out_offset, out_state);

    // "ISR 콜백" 컨텍스트
    struct isr_like_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.in_line = in_line;
    atomic_store(&ctx.running, 1);
    atomic_store(&ctx.toggle_flag, 0);
    ctx.debounce_us = 10000; // 10ms
    ctx.last_us = 0;

    // callback thread start
    pthread_t th;
    if (pthread_create(&th, NULL, isr_like_callback_thread, &ctx) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        gpiod_line_release(in_line);
        gpiod_line_release(out_line);
        gpiod_chip_close(chip);
        return 1;
    }

    // main loop: toggle_flag 확인 -> off -> output toggle
    while (atomic_load(&ctx.running)) {
        // if (1 == toggle_flag) { toggle_flag=0; output toggle; }
        if (atomic_load(&ctx.toggle_flag) == 1) {
            atomic_store(&ctx.toggle_flag, 0);

            out_state = !out_state;
            if (gpiod_line_set_value(out_line, out_state) < 0) {
                fprintf(stderr, "gpiod_line_set_value(offset=%u, value=%d) failed: %s\n",
                        out_offset, out_state, strerror(errno));
                break;
            }
	    int rb = gpiod_line_get_value(out_line);
	    printf("set=%d readback=%d\n", out_state, rb);

            printf("[MAIN] flag seen -> output(85) toggled to %d\n", out_state);
            fflush(stdout);
        }

        // 폴링 루프 CPU 점유 줄이기
        usleep(1000); // 1ms
    }

    // stop & cleanup
    atomic_store(&ctx.running, 0);
    pthread_join(th, NULL);

    gpiod_line_release(in_line);
    gpiod_line_release(out_line);
    gpiod_chip_close(chip);
    return 0;
}
