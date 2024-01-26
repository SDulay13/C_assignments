#include "u-lib.hh"

void process_main() {
    // Create a new process
    pid_t child_pid = sys_fork();
    if (child_pid == 0) {
        // This is the child process
        // Do nothing forever
        while (true) {
            sys_yield();
        }
    }
    else {
        // This is the parent process
        // Wait for 1 second
        sys_sleep(1);

        // Try to kill the child process
        int err = sys_kill(child_pid);
        if (err == 0) {
            console_printf(CPOS(1, 1), 0x0C00, "Process %d killed successfully\n", child_pid);
        }
        else {
            console_printf(CPOS(1, 1), 0x0C00, "Failed to kill process %d (error code %d)\n", child_pid, err);
        }

        // Do nothing forever
        while (true) {
            sys_yield();
        }
    }
}
