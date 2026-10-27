/*
 * main.c -- process setup and the single poll() loop.
 *
 * Everything the renderer does happens on this one thread: SSDP, HTTP, SOAP,
 * GENA and the signal pipe. No allocation after startup.
 */
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "event.h"
#include "http.h"
#include "log.h"
#include "renderer.h"
#include "soap.h"
#include "ssdp.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum source_kind {
    SRC_SIGNAL = 0,
    SRC_SSDP,
    SRC_LISTEN,
    SRC_CLIENT,
    SRC_GENA
};

static int g_sigpipe[2] = { -1, -1 };

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void on_signal(int sig)
{
    char c = (char)sig;
    ssize_t ignored = write(g_sigpipe[1], &c, 1);

    (void)ignored;
}

static int setup_signals(void)
{
    struct sigaction sa;

    if (pipe(g_sigpipe) != 0)
        return -1;
    set_nonblock(g_sigpipe[0]);
    set_nonblock(g_sigpipe[1]);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGPIPE, &sa, NULL);
    return 0;
}

static int add_poll(struct pollfd *pfds, int *kind, int *idx, int n,
                    int fd, short events, int k, int i)
{
    pfds[n].fd = fd;
    pfds[n].events = events;
    pfds[n].revents = 0;
    kind[n] = k;
    idx[n] = i;
    return n + 1;
}

int main(int argc, char **argv)
{
    struct dlna_config cfg;
    struct dlna_renderer renderer;
    struct gena gena;
    struct http_server http;
    struct ssdp ssdp;
    const struct dlna_arbiter *arb;
    int rc;
    int running = 1;

    dlna_config_init(&cfg);
    rc = dlna_config_parse(&cfg, argc, argv);
    if (rc != 0)
        return rc > 0 ? 0 : 1;
    log_set_level(cfg.log_level);

    if (cfg.mock_deny && !cfg.backend_mock) {
        log_error("--mock-deny only applies to --backend mock-sonosd");
        return 1;
    }
    if (cfg.backend_mock) {
        arb = dlna_arbiter_mock_sonosd();
        dlna_arbiter_mock_configure(cfg.mock_deny);
    } else {
        arb = dlna_arbiter_stub();
    }

    log_info("moggie-dlna %s starting (backend: %s)", DLNA_VERSION, arb->name);
    log_info("uuid:%s name=\"%s\"", cfg.uuid, cfg.friendly_name);
    log_info("location http://%s:%d/description.xml",
             cfg.advertise_ip, cfg.http_port);

    renderer_init(&renderer, &cfg, arb);
    gena_init(&gena, &cfg, &renderer);

    if (http_init(&http, &cfg, &renderer, &gena) != 0)
        return 1;
    if (ssdp_init(&ssdp, &cfg) != 0) {
        http_shutdown(&http);
        return 1;
    }
    if (setup_signals() != 0) {
        log_error("signals: %s", strerror(errno));
        ssdp_shutdown(&ssdp);
        http_shutdown(&http);
        return 1;
    }

    while (running) {
        struct pollfd pfds[2 + 1 + DLNA_MAX_CLIENTS + DLNA_GENA_MAX_OUT];
        int kind[2 + 1 + DLNA_MAX_CLIENTS + DLNA_GENA_MAX_OUT];
        int idx[2 + 1 + DLNA_MAX_CLIENTS + DLNA_GENA_MAX_OUT];
        int n = 0;
        int timeout = 1000;
        int i;
        int prc;
        int64_t now = now_ms();

        n = add_poll(pfds, kind, idx, n, g_sigpipe[0], POLLIN,
                     SRC_SIGNAL, 0);
        n = add_poll(pfds, kind, idx, n, ssdp.fd, POLLIN, SRC_SSDP, 0);
        n = add_poll(pfds, kind, idx, n, http.listen_fd, POLLIN,
                     SRC_LISTEN, 0);

        for (i = 0; i < DLNA_MAX_CLIENTS; i++) {
            if (http.conns[i].used)
                n = add_poll(pfds, kind, idx, n, http.conns[i].fd,
                             http_client_events(&http, i), SRC_CLIENT, i);
        }
        for (i = 0; i < DLNA_GENA_MAX_OUT; i++) {
            if (gena_out_used(&gena, i))
                n = add_poll(pfds, kind, idx, n, gena_out_fd(&gena, i),
                             gena_out_events(&gena, i), SRC_GENA, i);
        }

        {
            int t;

            t = ssdp_next_timeout_ms(&ssdp, now);
            if (t < timeout)
                timeout = t;
            t = http_next_timeout_ms(&http, now);
            if (t < timeout)
                timeout = t;
            t = gena_next_timeout_ms(&gena, now);
            if (t < timeout)
                timeout = t;
        }
        if (timeout < 0)
            timeout = 0;

        prc = poll(pfds, (nfds_t)n, timeout);
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll: %s", strerror(errno));
            break;
        }

        now = now_ms();
        for (i = 0; i < n; i++) {
            if (pfds[i].revents == 0)
                continue;

            switch (kind[i]) {
            case SRC_SIGNAL:
                running = 0;
                break;
            case SRC_SSDP:
                ssdp_read(&ssdp, now);
                break;
            case SRC_LISTEN:
                http_accept(&http, now);
                break;
            case SRC_CLIENT:
                http_client_ready(&http, idx[i], pfds[i].revents, now);
                break;
            case SRC_GENA:
                gena_out_ready(&gena, idx[i], pfds[i].revents, now);
                break;
            default:
                break;
            }
            if (!running)
                break;
        }
        if (!running)
            break;

        ssdp_tick(&ssdp, now);
        http_tick(&http, now);
        gena_tick(&gena, now);
        gena_flush(&gena);
    }

    log_info("shutting down");
    ssdp_byebye(&ssdp);
    renderer_shutdown(&renderer);
    gena_shutdown(&gena);
    http_shutdown(&http);
    ssdp_shutdown(&ssdp);

    if (g_sigpipe[0] >= 0)
        close(g_sigpipe[0]);
    if (g_sigpipe[1] >= 0)
        close(g_sigpipe[1]);
    log_info("stopped");
    return 0;
}
