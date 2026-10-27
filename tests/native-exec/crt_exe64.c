/* The entry of an arm64 executable, as bionic's crtbegin.c has it: _start
 * passes the initial stack (argc, argv, envp, auxv) to __libc_init with
 * main (topic native-exec; there is no NDK for arm64 here). */
typedef struct {
  void (**preinit_array)(void);
  void (**init_array)(void);
  void (**fini_array)(void);
} structors_array_t;

extern int main(int argc, char** argv, char** envp);
extern void __libc_init(void* raw_args, void (*onexit)(void),
                        int (*slingshot)(int, char**, char**),
                        structors_array_t const* const structors) __attribute__((noreturn));

__attribute__((visibility("hidden"), used)) void _start_main(void* raw_args) {
  static structors_array_t array = {0, 0, 0};
  __libc_init(raw_args, 0, &main, &array);
}

__asm__(".text\n.globl _start\n.type _start, %function\n_start:\n  mov x0, sp\n  b _start_main\n");
