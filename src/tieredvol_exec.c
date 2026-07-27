#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "tieredvol_types.h"
#include "tieredvol_exec.h"

int tv_exec_run(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return TV_ERR;
    if (pid == 0) {
        execvp(path, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int tv_exec_quiet(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return TV_ERR;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execvp(path, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int tv_exec_sudo(char *const argv[], int quiet) {
    pid_t pid = fork();
    if (pid < 0) return TV_ERR;
    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        }
        int i = 0;
        char *sudo_argv[64];
        sudo_argv[i++] = "sudo";
        for (char *const *a = argv; *a && i < 62; a++) sudo_argv[i++] = *a;
        sudo_argv[i] = NULL;
        execvp("sudo", sudo_argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int tv_exec_capture(const char *path, char *const argv[], char *out, size_t outsize) {
    int pfd[2];
    if (pipe(pfd) < 0) return TV_ERR;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return TV_ERR; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        close(pfd[1]);
        execvp(path, argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t total = 0;
    while (total < outsize - 1) {
        ssize_t n = read(pfd[0], out + total, outsize - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(pfd[0]);
    out[total] = 0;
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int tv_exec_with_stdin(const char *path, char *const argv[], const char *stdin_data) {
    int pfd[2];
    if (pipe(pfd) < 0) return TV_ERR;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return TV_ERR; }
    if (pid == 0) {
        close(pfd[1]);
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);
        execvp(path, argv);
        _exit(127);
    }
    close(pfd[0]);
    size_t len = strlen(stdin_data);
    const char *wp = stdin_data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t nw = write(pfd[1], wp, remaining);
        if (nw <= 0) break;
        wp += nw;
        remaining -= nw;
    }
    close(pfd[1]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
