#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t parent_from_c1 = 0;
static volatile sig_atomic_t parent_from_c2 = 0;
static volatile sig_atomic_t child1_turn = 0;
static volatile sig_atomic_t child2_turn = 0;
static volatile sig_atomic_t stop_flag = 0;

static void on_parent_from_c1(int signo) {
    (void)signo;
    parent_from_c1 = 1;
}

static void on_parent_from_c2(int signo) {
    (void)signo;
    parent_from_c2 = 1;
}

static void on_child1_turn(int signo) {
    (void)signo;
    child1_turn = 1;
}

static void on_child2_turn(int signo) {
    (void)signo;
    child2_turn = 1;
}

static void on_stop(int signo) {
    (void)signo;
    stop_flag = 1;
}

static void install_handler(int signo, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(signo, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static void wait_for_flag(volatile sig_atomic_t *flag) {
    while (!*flag && !stop_flag) {
        pause();
    }
}

static void write_int(int fd, int value) {
    ssize_t n = write(fd, &value, sizeof(value));
    if (n != (ssize_t)sizeof(value)) {
        perror("write");
        exit(EXIT_FAILURE);
    }
}

static int read_int(int fd, int *value) {
    ssize_t n = read(fd, value, sizeof(*value));
    if (n == 0) {
        return 0;
    }
    if (n != (ssize_t)sizeof(*value)) {
        perror("read");
        exit(EXIT_FAILURE);
    }
    return 1;
}

static void child_loop(const char *name, int read_fd, int write_fd, int max_value,
                       volatile sig_atomic_t *turn_flag, pid_t parent_pid, int notify_sig) {
    (void)max_value;
    while (!stop_flag) {
        wait_for_flag(turn_flag);
        if (stop_flag) {
            break;
        }
        *turn_flag = 0;

        int value;
        if (!read_int(read_fd, &value)) {
            break;
        }

        value += 1;
        fprintf(stdout, "[%s] 接收并+1 -> %d\n", name, value);
        fflush(stdout);

        write_int(write_fd, value);
        kill(parent_pid, notify_sig);
    }
}

int main(void) {
    int max_value;
    fprintf(stdout, "请输入最大常量：");
    fflush(stdout);
    if (scanf("%d", &max_value) != 1) {
        fprintf(stderr, "输入无效，需要整数。\n");
        return EXIT_FAILURE;
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    install_handler(SIGUSR1, on_parent_from_c1);
    install_handler(SIGUSR2, on_parent_from_c2);
    install_handler(SIGTERM, on_stop);

    pid_t child1 = fork();
    if (child1 < 0) {
        perror("fork child1");
        return EXIT_FAILURE;
    }

    if (child1 == 0) {
        install_handler(SIGUSR1, on_child1_turn);
        install_handler(SIGTERM, on_stop);
        /* 通知父进程：子进程1已完成初始化，可开始同步 */
        kill(getppid(), SIGUSR1);
        child_loop("子进程 1", pipefd[0], pipefd[1], max_value, &child1_turn, getppid(), SIGUSR1);
        close(pipefd[0]);
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }

    pid_t child2 = fork();
    if (child2 < 0) {
        perror("fork child2");
        kill(child1, SIGTERM);
        waitpid(child1, NULL, 0);
        return EXIT_FAILURE;
    }

    if (child2 == 0) {
        install_handler(SIGUSR2, on_child2_turn);
        install_handler(SIGTERM, on_stop);
        /* 通知父进程：子进程2已完成初始化，可开始同步 */
        kill(getppid(), SIGUSR2);
        child_loop("子进程 2", pipefd[0], pipefd[1], max_value, &child2_turn, getppid(), SIGUSR2);
        close(pipefd[0]);
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }

    /* 等待两个子进程就绪，避免首轮信号竞态 */
    wait_for_flag(&parent_from_c1);
    parent_from_c1 = 0;
    wait_for_flag(&parent_from_c2);
    parent_from_c2 = 0;

    int value = 0;
    fprintf(stdout, "[父进程] 初始值 0，向[子进程 1] 开球\n");
    fflush(stdout);
    write_int(pipefd[1], value);
    kill(child1, SIGUSR1);

    int expect_child = 1;
    int running = 1;
    while (running && !stop_flag) {
        if (expect_child == 1) {
            wait_for_flag(&parent_from_c1);
            parent_from_c1 = 0;
        } else {
            wait_for_flag(&parent_from_c2);
            parent_from_c2 = 0;
        }

        if (stop_flag) {
            break;
        }

        if (!read_int(pipefd[0], &value)) {
            break;
        }

        if (value > max_value) {
            running = 0;
            break;
        }

        value += 1;
        fprintf(stdout, "[父进程] 接收并+1 -> %d\n", value);
        fflush(stdout);

        if (value > max_value) {
            running = 0;
            break;
        }

        write_int(pipefd[1], value);
        if (expect_child == 1) {
            kill(child2, SIGUSR2);
            expect_child = 2;
        } else {
            kill(child1, SIGUSR1);
            expect_child = 1;
        }
    }

    kill(child1, SIGTERM);
    kill(child2, SIGTERM);

    close(pipefd[0]);
    close(pipefd[1]);

    waitpid(child1, NULL, 0);
    waitpid(child2, NULL, 0);

    fprintf(stdout, "数值已超过最大常量，游戏结束。\n");
    fflush(stdout);

    return EXIT_SUCCESS;
}
