#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

// Function pointer type for the real execve
typedef int (*execve_func_t)(const char *pathname, char *const argv[], char *const envp[]);

static void
send_execve_to_daemon(const char* pathname, char *const argv[], pid_t pid, pid_t ppid)
{
    const char* sock_path = getenv("SUDO_DAEMON_SOCK");
    if (!sock_path) {
        sock_path = "/run/sudo-process-recorder.sock";
    }
    
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;
    
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    
    char msg[4096];
    int offset = snprintf(
        msg,
        sizeof(msg), 
        "{\"type\":\"execve\",\"pid\":%d,\"ppid\":%d,\"program\":\"%s\",\"args\":[",
        pid,
        ppid,
        pathname ? pathname : ""
    );
    
    if (argv && offset < sizeof(msg)) {
        for (int i = 0; argv[i] != NULL && offset < sizeof(msg) - 100; i++) {
            if (i > 0) offset += snprintf(msg + offset, sizeof(msg) - offset, ",");
            offset += snprintf(msg + offset, sizeof(msg) - offset, "\"%s\"", argv[i]);
        }
    }
    
    if (offset < sizeof(msg)) {
        offset += snprintf(msg + offset, sizeof(msg) - offset, "]}");
    }
    
    sendto(fd, msg, strlen(msg), 0, (struct sockaddr*)&addr, sizeof(addr));
    close(fd);
}

static void
log_execve_to_file(const char* pathname, char *const argv[], pid_t pid, pid_t ppid)
{
    const char* log_file = getenv("SUDO_LOG_FILE");
    if (!log_file || log_file[0] == '\0') {
        return;
    }
    
    FILE* log_fp = NULL;
    FILE* output = NULL;
    
    if (strcmp(log_file, "stdout") == 0) {
        output = stdout;
    } else {
        log_fp = fopen(log_file, "a");
        output = log_fp;
    }
    
    if (!output) {
        return;
    }
    
    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", localtime(&now));
    
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "unknown");
    }
    
    uid_t uid = getuid();
    char user[256];
    snprintf(user, sizeof(user), "%d", uid);
    
    fprintf(
        output,
        "[%s] Sub-process %d (ppid=%d, user=%s, cwd=%s): %s", 
        timestamp,
        pid,
        ppid,
        user,
        cwd,
        pathname ? pathname : "(null)"
    );
    
    if (argv && argv[1]) {
        fprintf(output, " ");
        for (int i = 1; argv[i] != NULL && i < 10; i++) {
            fprintf(output, "%s%s", i > 1 ? " " : "", argv[i]);
        }
    }
    fprintf(output, "\n");
    
    if (log_fp) {
        fflush(log_fp);
        fclose(log_fp);
    }
}

// Intercept execve calls
int execve(const char *pathname, char *const argv[], char *const envp[]) {
    execve_func_t real_execve = (execve_func_t)dlsym(RTLD_NEXT, "execve");
    if (!real_execve) {
        fprintf(stderr, "Error: Unable to find real execve: %s\n", dlerror());
        return -1;
    }

    pid_t pid = getpid();
    pid_t ppid = getppid();

    send_execve_to_daemon(pathname, argv, pid, ppid);
    log_execve_to_file(pathname, argv, pid, ppid);

    return real_execve(pathname, argv, envp);
}

int execv(const char *pathname, char *const argv[]) {
    extern char **environ;
    return execve(pathname, argv, environ);
}
