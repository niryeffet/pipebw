#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>

ssize_t min_bw = 1000;
long secs = 5;

void my_exit(int code) {
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) & ~O_NONBLOCK);
    fcntl(STDOUT_FILENO, F_SETFL, fcntl(STDOUT_FILENO, F_GETFL) & ~O_NONBLOCK);
    exit(code);
}

void perror_die(char* msg) {
    perror(msg);
    my_exit(EXIT_FAILURE);
}

int check(int ret, char *msg) {
    if (ret == -1) perror_die(msg);
    return ret;
}

void usage() {
    dprintf(STDERR_FILENO, "Usage:\n-b <bytes> minimum bytes per (default: %ld bytes)\n-s <secs> seconds (default: %ld seconds)\n", min_bw, secs);
    exit(EXIT_SUCCESS);
}

int main(int argc, char* argv[]) {
    signal(SIGHUP, SIG_IGN);

    int opt; 
    while((opt = getopt(argc, argv, "b:s:h")) != -1)
        switch(opt) {
            case 'b':
                min_bw = strtol(optarg, NULL, 10);
                break;
            case 's':
                secs = strtol(optarg, NULL, 10);
                break;
            case 'h':
            case '?':
                usage();             
        }; 
    if (optind < argc) usage();

    check(fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK), "fcntl");
    check(fcntl(STDOUT_FILENO, F_SETFL, fcntl(STDOUT_FILENO, F_GETFL) | O_NONBLOCK), "fcntl");

    int epfd = check(epoll_create(3), "epoll_create");

    int tmfd = check(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK), "timerfd_create");
    struct itimerspec when = {{secs, 0}, {secs, 0}};
    check(timerfd_settime(tmfd, 0, &when, NULL), "timerfd_settime");

    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLET,
        .data.fd = tmfd
    };
    check(epoll_ctl(epfd, EPOLL_CTL_ADD, tmfd, &ev), "epoll_ctl");

    ev.data.fd = STDIN_FILENO;
    check(epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &ev), "epoll_ctl");

    ev.events = 0;
    ev.data.fd = STDOUT_FILENO;
    check(epoll_ctl(epfd, EPOLL_CTL_ADD, STDOUT_FILENO, &ev), "epoll_ctl");

    char tmbuf[64];
    char buf[4096];

    ssize_t b_read = 0, b_wrote = 0, bytes = 0;

    while(epoll_wait(epfd, &ev, 1, -1) == 1) {
        if (ev.data.fd == tmfd) {
            if (bytes < min_bw) {
                dprintf(STDERR_FILENO, "Too slow\n");
                my_exit(EXIT_FAILURE);
            }
            check(read(tmfd, tmbuf, sizeof(tmbuf)), "read timerfd");
            bytes = 0;
            continue;
        }
        for(;;) {
            if (b_read == 0) {
                b_read = read(STDIN_FILENO, buf, sizeof(buf));
                if (b_read > 0) bytes += b_read;
                else if (b_read == -1) {
                    if (errno == EAGAIN) {
                        b_read = 0;
                        break;
                    } else perror_die("read");
                } else my_exit(EXIT_SUCCESS);
            }
            if (b_read) {
                int w = write(STDOUT_FILENO, buf + b_wrote, b_read - b_wrote);
                if (w != -1) b_wrote += w;
                else {
                    if (errno == EAGAIN) {
                        ev.events = EPOLLOUT | EPOLLONESHOT;
                        ev.data.fd = STDOUT_FILENO;
                        check(epoll_ctl(epfd, EPOLL_CTL_MOD, STDOUT_FILENO, &ev), "epoll_ctl");
                        break;
                    } else perror_die("write");
                }
                if (b_wrote == b_read) b_read = b_wrote = 0;
            }
        }
    }
    perror_die("epoll_wait");
}