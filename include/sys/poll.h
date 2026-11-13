/*
 * Minimal sys/poll.h for Windows
 * Implements poll() using select()
 */
#ifndef SYS_POLL_H
#define SYS_POLL_H

#include <winsock2.h>
#include <sys/types.h>

#define POLLIN    0x001
#define POLLOUT   0x004
#define POLLERR   0x008
#define POLLHUP   0x010
#define POLLNVAL  0x020

struct pollfd {
    SOCKET fd;
    short events;
    short revents;
};

static inline int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (nfds <= 0) return 0;
    if (fds == NULL) return -1;

    fd_set readfds;
    fd_set writefds;
    fd_set exceptfds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);

    int max_fd = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd > max_fd) max_fd = fds[i].fd;
        if (fds[i].events & (POLLIN | POLLERR | POLLHUP))
            FD_SET(fds[i].fd, &readfds);
        if (fds[i].events & POLLOUT)
            FD_SET(fds[i].fd, &writefds);
        if (fds[i].events & (POLLERR | POLLHUP))
            FD_SET(fds[i].fd, &exceptfds);
    }

    if (max_fd > FD_SETSIZE) return -1;

    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;

    int ret = select(max_fd + 1, &readfds, &writefds, &exceptfds, timeout < 0 ? NULL : &tv);
    if (ret < 0) return ret;

    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (FD_ISSET(fds[i].fd, &readfds))
            fds[i].revents |= POLLIN;
        if (FD_ISSET(fds[i].fd, &writefds))
            fds[i].revents |= POLLOUT;
        if (FD_ISSET(fds[i].fd, &exceptfds))
            fds[i].revents |= (POLLERR | POLLHUP);
    }

    return ret;
}

#endif
