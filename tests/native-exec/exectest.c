/* A native program for NativeExecTest (topic native-exec): built as an
 * arm64-v8a and an armeabi-v7a PIE executable (mk/topic-native-exec.mk) and
 * run by the test through ProcessBuilder. argv[1] says what to do. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("exectest: no command\n");
    return 2;
  }
  const char* cmd = argv[1];
  if (!strcmp(cmd, "echo")) {
    for (int i = 2; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
    return 0;
  }
  if (!strcmp(cmd, "cat")) {  /* stdin to stdout, the count to stderr */
    char buf[256];
    ssize_t n;
    long total = 0;
    while ((n = read(0, buf, sizeof buf)) > 0) {
      if (write(1, buf, n) != n) return 4;
      total += n;
    }
    fprintf(stderr, "cat: %ld bytes\n", total);
    return 0;
  }
  if (!strcmp(cmd, "exit") && argc > 2) return atoi(argv[2]);
  if (!strcmp(cmd, "env") && argc > 2) {
    const char* v = getenv(argv[2]);
    printf("%s=%s\n", argv[2], v ? v : "(unset)");
    return 0;
  }
  if (!strcmp(cmd, "cwd")) {
    char b[512];
    printf("%s\n", getcwd(b, sizeof b) ? b : "?");
    return 0;
  }
  if (!strcmp(cmd, "stderr")) {
    fprintf(stderr, "to stderr\n");
    printf("to stdout\n");
    return 3;
  }
  if (!strcmp(cmd, "pid")) {
    printf("pid %d ppid %d\n", getpid(), getppid());
    return 0;
  }
  if (!strcmp(cmd, "abort")) abort();
  if (!strcmp(cmd, "sleep") && argc > 2) {
    sleep(atoi(argv[2]));
    return 0;
  }
  if (!strcmp(cmd, "file") && argc > 3) {  /* a file written relative to the cwd */
    FILE* f = fopen(argv[2], "w");
    if (!f) return 5;
    fprintf(f, "%s", argv[3]);
    fclose(f);
    return 0;
  }
  if (!strcmp(cmd, "exitcall") && argc > 2) exit(atoi(argv[2]));
  if (!strcmp(cmd, "spawnself") && argc > 3) {  /* fork + exec a child (self), wait */
    char* a[] = {argv[0], (char*)"exit", argv[3], NULL};
    pid_t pid = fork();
    if (pid < 0) { printf("fork failed: %s\n", strerror(errno)); return 20; }
    if (pid == 0) { execv(argv[0], a); _exit(127); }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) { printf("wait failed: %s\n", strerror(errno)); return 21; }
    printf("child exited %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
  }
  if (!strcmp(cmd, "exec") && argc > 2) {  /* the process runs argv[2..] instead */
    setenv("EXECED", "yes", 1);
    execv(argv[2], argv + 2);
    printf("execv: %s\n", strerror(errno));
    return 6;
  }
  if (!strcmp(cmd, "qemu") && argc > 2) {  /* mimic qemu-user: [-L DIR] PROG ARGS, then exec PROG */
    int i = 2;
    if (i + 1 < argc && !strcmp(argv[i], "-L")) i += 2;  /* -L DIR: our exec layer reads it, we skip it */
    if (i >= argc) { printf("qemu: no program\n"); return 6; }
    execv(argv[i], argv + i);
    printf("qemu execv %s: %s\n", argv[i], strerror(errno));
    return 6;
  }
  if (!strcmp(cmd, "closefds")) {  /* as daemons do: its parent's stay open */
    int null = open("/dev/null", O_RDWR);
    for (int fd = 3; fd < 200; fd++)
      if (fd != null) dup2(null, fd);
    for (int fd = 3; fd < 256; fd++) close(fd);
    int f = open("/dev/null", O_RDONLY);
    printf("closed, reopened as %d\n", f);
    return 0;
  }
  printf("exectest got: %s\n", cmd);  /* argv[1]; binfmt_misc passes the matched file here */
  return 2;
}
