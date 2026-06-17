#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAIL_MARKER (-1)

static volatile sig_atomic_t parent_from_c1 = 0;
static volatile sig_atomic_t parent_from_c2 = 0;
static volatile sig_atomic_t child1_turn = 0;
static volatile sig_atomic_t child2_turn = 0;
static volatile sig_atomic_t stop_flag = 0;
static volatile sig_atomic_t timeout_flag = 0;

static int fail_percent = 0;

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

static void on_timeout(int signo) {
    (void)signo;
    timeout_flag = 1;
}

static void install_handler(int signo, void (*handler)(int), int restart) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = restart ? SA_RESTART : 0;
    if (sigaction(signo, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static void wait_for_flag(volatile sig_atomic_t *flag) {
    while (!*flag && !stop_flag && !timeout_flag) {
        pause();
    }
}

static void write_int(int fd, int value) {
    ssize_t n;
    do {
        n = write(fd, &value, sizeof(value));
    } while (n == -1 && errno == EINTR);
    if (n != (ssize_t)sizeof(value)) {
        perror("write");
        exit(EXIT_FAILURE);
    }
}

static int read_int(int fd, int *value) {
    ssize_t n;
    do {
        n = read(fd, value, sizeof(*value));
    } while (n == -1 && errno == EINTR && !timeout_flag && !stop_flag);
    if (n == 0) {
        return 0;
    }
    if (n == -1) {
        if (errno == EINTR) {
            return -1;
        }
        perror("read");
        exit(EXIT_FAILURE);
    }
    if (n != (ssize_t)sizeof(*value)) {
        fprintf(stderr, "partial read\n");
        exit(EXIT_FAILURE);
    }
    return 1;
}

static void child_loop(const char *name, int read_fd, int write_fd,
                       volatile sig_atomic_t *turn_flag,
                       pid_t parent_pid, int notify_sig) {
    while (!stop_flag && !timeout_flag) {
        wait_for_flag(turn_flag);
        if (stop_flag || timeout_flag) {
            break;
        }
        *turn_flag = 0;

        int value;
        int rc = read_int(read_fd, &value);
        if (rc <= 0) {
            break;
        }

        if (fail_percent > 0 && (rand() % 100) < fail_percent) {
            fprintf(stdout, "[%s] 随机失败！\n", name);
            fflush(stdout);
            write_int(write_fd, FAIL_MARKER);
            kill(parent_pid, notify_sig);
            break;
        }

        value += 1;
        fprintf(stdout, "[%s] 接收并+1 -> %d\n", name, value);
        fflush(stdout);

        write_int(write_fd, value);
        kill(parent_pid, notify_sig);
    }
}

enum round_result_t {
    RESULT_WIN,
    RESULT_TIMEOUT,
    RESULT_RANDOM_FAIL,
    RESULT_INTERRUPTED
};

struct round_outcome {
    enum round_result_t result;
    int pass_count;
};

static struct round_outcome run_round(int round_num, int max_passes, int timeout_ms) {
    struct round_outcome outcome = {RESULT_INTERRUPTED, 0};

    fprintf(stdout, "\n===== 第 %d 局开始 =====\n", round_num);
    fflush(stdout);

    parent_from_c1 = 0;
    parent_from_c2 = 0;
    child1_turn = 0;
    child2_turn = 0;
    timeout_flag = 0;

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    install_handler(SIGUSR1, on_parent_from_c1, 1);
    install_handler(SIGUSR2, on_parent_from_c2, 1);
    install_handler(SIGTERM, on_stop, 1);
    install_handler(SIGALRM, on_timeout, 0);

    pid_t child1 = fork();
    if (child1 < 0) {
        perror("fork child1");
        close(pipefd[0]);
        close(pipefd[1]);
        return outcome;
    }

    if (child1 == 0) {
        install_handler(SIGUSR1, on_child1_turn, 1);
        install_handler(SIGTERM, on_stop, 1);
        kill(getppid(), SIGUSR1);
        child_loop("子进程 1", pipefd[0], pipefd[1],
                   &child1_turn, getppid(), SIGUSR1);
        close(pipefd[0]);
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }

    pid_t child2 = fork();
    if (child2 < 0) {
        perror("fork child2");
        kill(child1, SIGTERM);
        waitpid(child1, NULL, 0);
        close(pipefd[0]);
        close(pipefd[1]);
        return outcome;
    }

    if (child2 == 0) {
        install_handler(SIGUSR2, on_child2_turn, 1);
        install_handler(SIGTERM, on_stop, 1);
        kill(getppid(), SIGUSR2);
        child_loop("子进程 2", pipefd[0], pipefd[1],
                   &child2_turn, getppid(), SIGUSR2);
        close(pipefd[0]);
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }

    wait_for_flag(&parent_from_c1);
    parent_from_c1 = 0;
    wait_for_flag(&parent_from_c2);
    parent_from_c2 = 0;

    struct itimerval timer;
    timer.it_value.tv_sec = timeout_ms / 1000;
    timer.it_value.tv_usec = (timeout_ms % 1000) * 1000;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, NULL);

    int value = 0;
    fprintf(stdout, "[父进程] 初始值 0，向[子进程 1] 开球\n");
    fflush(stdout);
    write_int(pipefd[1], value);
    kill(child1, SIGUSR1);

    int expect_child = 1;
    int pass_count = 0;
    int running = 1;

    while (running && !stop_flag && !timeout_flag) {
        if (expect_child == 1) {
            wait_for_flag(&parent_from_c1);
            parent_from_c1 = 0;
        } else {
            wait_for_flag(&parent_from_c2);
            parent_from_c2 = 0;
        }

        if (timeout_flag) {
            outcome.result = RESULT_TIMEOUT;
            running = 0;
            break;
        }
        if (stop_flag) {
            running = 0;
            break;
        }

        int rc = read_int(pipefd[0], &value);
        if (rc == -1) {
            if (timeout_flag) {
                outcome.result = RESULT_TIMEOUT;
            }
            running = 0;
            break;
        }
        if (rc == 0) {
            if (timeout_flag) {
                outcome.result = RESULT_TIMEOUT;
            }
            running = 0;
            break;
        }

        if (value == FAIL_MARKER) {
            outcome.result = RESULT_RANDOM_FAIL;
            running = 0;
            break;
        }

        pass_count++;
        if (pass_count >= max_passes) {
            outcome.result = RESULT_WIN;
            running = 0;
            break;
        }

        if (fail_percent > 0 && (rand() % 100) < fail_percent) {
            fprintf(stdout, "[父进程] 随机失败！\n");
            fflush(stdout);
            outcome.result = RESULT_RANDOM_FAIL;
            running = 0;
            break;
        }

        value += 1;
        fprintf(stdout, "[父进程] 接收并+1 -> %d\n", value);
        fflush(stdout);

        pass_count++;
        if (pass_count >= max_passes) {
            outcome.result = RESULT_WIN;
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

    if (outcome.result == RESULT_INTERRUPTED && timeout_flag) {
        outcome.result = RESULT_TIMEOUT;
    }

    struct itimerval cancel = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &cancel, NULL);

    kill(child1, SIGTERM);
    kill(child2, SIGTERM);
    close(pipefd[0]);
    close(pipefd[1]);
    waitpid(child1, NULL, 0);
    waitpid(child2, NULL, 0);

    outcome.pass_count = pass_count;

    const char *result_str;
    switch (outcome.result) {
    case RESULT_WIN:
        result_str = "胜利（达到最大传球次数）";
        break;
    case RESULT_TIMEOUT:
        result_str = "失败（超时）";
        break;
    case RESULT_RANDOM_FAIL:
        result_str = "失败（随机失败）";
        break;
    default:
        result_str = "中断";
        break;
    }
    fprintf(stdout, "----- 第 %d 局结束：%s，传球次数 %d -----\n",
            round_num, result_str, pass_count);
    fflush(stdout);

    return outcome;
}

int main(void) {
    int num_rounds, max_passes, timeout_ms;

    fprintf(stdout, "请输入局数：");
    fflush(stdout);
    if (scanf("%d", &num_rounds) != 1 || num_rounds <= 0) {
        fprintf(stderr, "输入无效。\n");
        return EXIT_FAILURE;
    }

    fprintf(stdout, "请输入最大传球次数：");
    fflush(stdout);
    if (scanf("%d", &max_passes) != 1 || max_passes <= 0) {
        fprintf(stderr, "输入无效。\n");
        return EXIT_FAILURE;
    }

    fprintf(stdout, "请输入超时毫秒：");
    fflush(stdout);
    if (scanf("%d", &timeout_ms) != 1 || timeout_ms <= 0) {
        fprintf(stderr, "输入无效。\n");
        return EXIT_FAILURE;
    }

    fprintf(stdout, "请输入随机失败概率(0-100%%)：");
    fflush(stdout);
    if (scanf("%d", &fail_percent) != 1 || fail_percent < 0 || fail_percent > 100) {
        fprintf(stderr, "输入无效。\n");
        return EXIT_FAILURE;
    }

    srand((unsigned)time(NULL));

    int wins = 0;
    int losses = 0;
    int total_passes = 0;
    int timeout_count = 0;
    int random_fail_count = 0;
    int interrupted_count = 0;

    for (int round = 1; round <= num_rounds && !stop_flag; round++) {
        struct round_outcome o = run_round(round, max_passes, timeout_ms);
        total_passes += o.pass_count;

        switch (o.result) {
        case RESULT_WIN:
            wins++;
            break;
        case RESULT_TIMEOUT:
            losses++;
            timeout_count++;
            break;
        case RESULT_RANDOM_FAIL:
            losses++;
            random_fail_count++;
            break;
        default:
            losses++;
            interrupted_count++;
            break;
        }
    }

    int played = wins + losses;
    fprintf(stdout, "\n====== 比赛总结 ======\n");
    fprintf(stdout, "总局数：%d\n", num_rounds);
    fprintf(stdout, "已玩局数：%d\n", played);
    fprintf(stdout, "胜局：%d  负局：%d\n", wins, losses);
    if (played > 0) {
        fprintf(stdout, "胜率：%.1f%%\n", 100.0 * wins / played);
        fprintf(stdout, "平均传球次数：%.1f\n", (double)total_passes / played);
    }
    fprintf(stdout, "失败类型分布：\n");
    fprintf(stdout, "  超时：%d\n", timeout_count);
    fprintf(stdout, "  随机失败：%d\n", random_fail_count);
    if (interrupted_count > 0) {
        fprintf(stdout, "  中断：%d\n", interrupted_count);
    }
    fflush(stdout);

    return EXIT_SUCCESS;
}
