/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */
/*
 * Internal child process machinery for use by other parts of libxl
 */

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"

/*
 * carefd arrangements
 *
 * carefd_begin and _unlock take out the no_forking lock, which we
 * also take and release in our pthread_atfork handlers.  So when this
 * lock is held the whole process cannot fork.  We therefore protect
 * our fds from leaking into children made by other threads.
 *
 * We maintain a list of all the carefds, so that if the application
 * wants to fork a long-running but non-execing child, we can close
 * them all.
 *
 * So the record function sets CLOEXEC for the benefit of execing
 * children, and makes a note of the fd for the benefit of non-execing
 * ones.
 */

struct libxl__carefd {
    LIBXL_LIST_ENTRY(libxl__carefd) entry;
    int fd;
};

static pthread_mutex_t no_forking = PTHREAD_MUTEX_INITIALIZER;
static int atfork_registered;
static LIBXL_LIST_HEAD(, libxl__carefd) carefds =
    LIBXL_LIST_HEAD_INITIALIZER(carefds);

/* non-null iff installed, protected by no_forking */
static libxl_ctx *sigchld_owner;
static struct sigaction sigchld_saved_action;

static void sigchld_removehandler_core(void);

static void atfork_lock(void)
{
    int r = pthread_mutex_lock(&no_forking);
    assert(!r);
}

static void atfork_unlock(void)
{
    int r = pthread_mutex_unlock(&no_forking);
    assert(!r);
}

int libxl__atfork_init(libxl_ctx *ctx)
{
    int r, rc;
    
    atfork_lock();
    if (atfork_registered) { rc = 0; goto out; }

    r = pthread_atfork(atfork_lock, atfork_unlock, atfork_unlock);
    if (r) {
        assert(r == ENOMEM);
        libxl__alloc_failed(ctx, __func__, 0,0);
    }

    atfork_registered = 1;
    rc = 0;
 out:
    atfork_unlock();
    return rc;
}

void libxl__carefd_begin(void) { atfork_lock(); }
void libxl__carefd_unlock(void) { atfork_unlock(); }

libxl__carefd *libxl__carefd_record(libxl_ctx *ctx, int fd)
{
    libxl__carefd *cf = 0;

    libxl_fd_set_cloexec(ctx, fd, 1);
    cf = libxl__zalloc(&ctx->nogc_gc, sizeof(*cf));
    cf->fd = fd;
    LIBXL_LIST_INSERT_HEAD(&carefds, cf, entry);
    return cf;
}

libxl__carefd *libxl__carefd_opened(libxl_ctx *ctx, int fd)
{
    libxl__carefd *cf = 0;

    cf = libxl__carefd_record(ctx, fd);
    libxl__carefd_unlock();
    return cf;
}

void libxl_postfork_child_noexec(libxl_ctx *ctx)
{
    libxl__carefd *cf, *cf_tmp;
    int r;

    atfork_lock();

    LIBXL_LIST_FOREACH_SAFE(cf, &carefds, entry, cf_tmp) {
        if (cf->fd >= 0) {
            r = close(cf->fd);
            if (r)
                LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_WARNING,
                                 "failed to close fd=%d"
                                 " (perhaps of another libxl ctx)", cf->fd);
        }
        free(cf);
    }
    LIBXL_LIST_INIT(&carefds);

    if (sigchld_owner)
        sigchld_removehandler_core();

    atfork_unlock();
}

int libxl__carefd_close(libxl__carefd *cf)
{
    if (!cf) return 0;
    int r = cf->fd < 0 ? 0 : close(cf->fd);
    int esave = errno;
    atfork_lock();
    LIBXL_LIST_REMOVE(cf, entry);
    atfork_unlock();
    free(cf);
    errno = esave;
    return r;
}

int libxl__carefd_fd(const libxl__carefd *cf)
{
    if (!cf) return -1;
    return cf->fd;
}

/*
 * Actual child process handling
 */

static void sigchld_selfpipe_handler(libxl__egc *egc, libxl__ev_fd *ev,
                                     int fd, short events, short revents);

static void sigchld_handler(int signo)
{
    int esave = errno;
    int e = libxl__self_pipe_wakeup(sigchld_owner->sigchld_selfpipe[1]);
    assert(!e); /* errors are probably EBADF, very bad */
    errno = esave;
}

static void sigchld_removehandler_core(void)
{
    struct sigaction was;
    int r;
    
    r = sigaction(SIGCHLD, &sigchld_saved_action, &was);
    assert(!r);
    assert(!(was.sa_flags & SA_SIGINFO));
    assert(was.sa_handler == sigchld_handler);
    sigchld_owner = 0;
}

void libxl__sigchld_removehandler(libxl__gc *gc) /* non-reentrant */
{
    int rc;

    atfork_lock();
    if (sigchld_owner == CTX)
        sigchld_removehandler_core();
    atfork_unlock();

    if (libxl__ev_fd_isregistered(&CTX->sigchld_selfpipe_efd)) {
        rc = libxl__ev_fd_modify(gc, &CTX->sigchld_selfpipe_efd, 0);
        if (rc)
            libxl__ev_fd_deregister(gc, &CTX->sigchld_selfpipe_efd);
    }
}

int libxl__sigchld_installhandler(libxl__gc *gc) /* non-reentrant */
{
    int r, rc;

    if (CTX->sigchld_selfpipe[0] < 0) {
        r = pipe(CTX->sigchld_selfpipe);
        if (r) {
            CTX->sigchld_selfpipe[0] = -1;
            LIBXL__LOG_ERRNO(CTX, LIBXL__LOG_ERROR,
                             "failed to create sigchld pipe");
            rc = ERROR_FAIL;
            goto out;
        }
    }
    if (!libxl__ev_fd_isregistered(&CTX->sigchld_selfpipe_efd)) {
        rc = libxl__ev_fd_register(gc, &CTX->sigchld_selfpipe_efd,
                                   sigchld_selfpipe_handler,
                                   CTX->sigchld_selfpipe[0], POLLIN);
        if (rc) goto out;
    } else {
        rc = libxl__ev_fd_modify(gc, &CTX->sigchld_selfpipe_efd, POLLIN);
        if (rc) goto out;
    }

    atfork_lock();
    if (sigchld_owner != CTX) {
        struct sigaction ours;

        assert(!sigchld_owner);
        sigchld_owner = CTX;

        memset(&ours,0,sizeof(ours));
        ours.sa_handler = sigchld_handler;
        sigemptyset(&ours.sa_mask);
        ours.sa_flags = SA_NOCLDSTOP | SA_RESTART;
        r = sigaction(SIGCHLD, &ours, &sigchld_saved_action);
        assert(!r);

        assert(((void)"application must negotiate with libxl about SIGCHLD",
                !(sigchld_saved_action.sa_flags & SA_SIGINFO) &&
                (sigchld_saved_action.sa_handler == SIG_DFL ||
                 sigchld_saved_action.sa_handler == SIG_IGN)));
    }
    atfork_unlock();

    rc = 0;
 out:
    return rc;
}

static bool chldmode_ours(libxl_ctx *ctx, bool creating)
{
    switch (ctx->childproc_hooks->chldowner) {
    case libxl_sigchld_owner_libxl:
        return creating || !LIBXL_LIST_EMPTY(&ctx->children);
    case libxl_sigchld_owner_mainloop:
        return 0;
    case libxl_sigchld_owner_libxl_always:
        return 1;
    }
    abort();
}

static void perhaps_removehandler(libxl__gc *gc)
{
    if (!chldmode_ours(CTX, 0))
        libxl__sigchld_removehandler(gc);
}

static int perhaps_installhandler(libxl__gc *gc, bool creating)
{
    int rc;

    if (chldmode_ours(CTX, creating)) {
        rc = libxl__sigchld_installhandler(gc);
        if (rc) return rc;
    }
    return 0;
}

static int childproc_reaped(libxl__egc *egc, pid_t pid, int status)
{
    EGC_GC;
    libxl__ev_child *ch;

    LIBXL_LIST_FOREACH(ch, &CTX->children, entry)
        if (ch->pid == pid)
            goto found;

    /* not found */
    return ERROR_UNKNOWN_CHILD;

 found:
    LIBXL_LIST_REMOVE(ch, entry);
    ch->pid = -1;
    ch->callback(egc, ch, pid, status);

    perhaps_removehandler(gc);

    return 0;
}

int libxl_childproc_reaped(libxl_ctx *ctx, pid_t pid, int status)
{
    EGC_INIT(ctx);
    CTX_LOCK;
    int rc = childproc_reaped(egc, pid, status);
    CTX_UNLOCK;
    EGC_FREE;
    return rc;
}

static void sigchld_selfpipe_handler(libxl__egc *egc, libxl__ev_fd *ev,
                                     int fd, short events, short revents)
{
    /* May make callbacks into the application for child processes.
     * So, this function may unlock and relock the CTX.  This is OK
     * because event callback functions are always called with the CTX
     * locked exactly once, and from code which copes with reentrancy.
     * (See also the comment in afterpoll_internal.) */
    EGC_GC;

    int selfpipe = CTX->sigchld_selfpipe[0];

    if (revents & ~POLLIN) {
        LOG(ERROR, "unexpected poll event 0x%x on SIGCHLD self pipe", revents);
        LIBXL__EVENT_DISASTER(egc,
                              "unexpected poll event on SIGCHLD self pipe",
                              0, 0);
    }
    assert(revents & POLLIN);

    int e = libxl__self_pipe_eatall(selfpipe);
    if (e) LIBXL__EVENT_DISASTER(egc, "read sigchld pipe", e, 0);

    while (chldmode_ours(CTX, 0) /* in case the app changes the mode */) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) return;

        if (pid == -1) {
            if (errno == ECHILD) return;
            if (errno == EINTR) continue;
            LIBXL__EVENT_DISASTER(egc, "waitpid() failed", errno, 0);
            return;
        }

        int rc = childproc_reaped(egc, pid, status);

        if (rc) {
            if (CTX->childproc_hooks->reaped_callback) {
                CTX_UNLOCK;
                rc = CTX->childproc_hooks->reaped_callback
                        (pid, status, CTX->childproc_user);
                CTX_LOCK;
                if (rc != 0 && rc != ERROR_UNKNOWN_CHILD) {
                    char disasterbuf[200];
                    snprintf(disasterbuf, sizeof(disasterbuf), " reported by"
                             " libxl_childproc_hooks->reaped_callback"
                             " (for pid=%lu, status=%d; error code %d)",
                             (unsigned long)pid, status, rc);
                    LIBXL__EVENT_DISASTER(egc, disasterbuf, 0, 0);
                    return;
                }
            } else {
                rc = ERROR_UNKNOWN_CHILD;
            }
            if (rc)
                libxl_report_child_exitstatus(CTX, XTL_WARN,
                                "unknown child", (long)pid, status);
        }
    }
}

pid_t libxl__ev_child_fork(libxl__gc *gc, libxl__ev_child *ch,
                           libxl__ev_child_callback *death)
{
    CTX_LOCK;
    int rc;

    perhaps_installhandler(gc, 1);

    pid_t pid =
        CTX->childproc_hooks->fork_replacement
        ? CTX->childproc_hooks->fork_replacement(CTX->childproc_user)
        : fork();
    if (pid == -1) {
        LOGE(ERROR, "fork failed");
        rc = ERROR_FAIL;
        goto out;
    }

    if (!pid) {
        /* woohoo! */
        if (CTX->xsh) {
            xs_daemon_destroy_postfork(CTX->xsh);
            CTX->xsh = NULL; /* turns mistakes into crashes */
        }
        /* Yes, CTX is left locked in the child. */
        return 0;
    }

    ch->pid = pid;
    ch->callback = death;
    LIBXL_LIST_INSERT_HEAD(&CTX->children, ch, entry);
    rc = pid;

 out:
    perhaps_removehandler(gc);
    CTX_UNLOCK;
    return rc;
}

void libxl_childproc_setmode(libxl_ctx *ctx, const libxl_childproc_hooks *hooks,
                             void *user)
{
    GC_INIT(ctx);
    CTX_LOCK;

    assert(LIBXL_LIST_EMPTY(&CTX->children));

    if (!hooks)
        hooks = &libxl__childproc_default_hooks;

    ctx->childproc_hooks = hooks;
    ctx->childproc_user = user;

    perhaps_removehandler(gc);
    perhaps_installhandler(gc, 0); /* idempotent, ok to ignore errors for now */

    CTX_UNLOCK;
    GC_FREE;
}

const libxl_childproc_hooks libxl__childproc_default_hooks = {
    libxl_sigchld_owner_libxl, 0, 0
};

int libxl__ev_child_xenstore_reopen(libxl__gc *gc, const char *what) {
    int rc;

    assert(!CTX->xsh);
    CTX->xsh = xs_daemon_open();
    if (!CTX->xsh) {
        LOGE(ERROR, "%s: xenstore reopen failed", what);
        rc = ERROR_FAIL;  goto out;
    }

    libxl_fd_set_cloexec(CTX, xs_fileno(CTX->xsh), 1);

    return 0;

 out:
    return rc;
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
