// bgproc.cpp
// Simple background/daemon process with pidfile, logging and graceful shutdown.
// Compile: g++ -std=c++11 -O2 -o bgproc bgproc.cpp -pthread

#include <iostream>
#include <fstream>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/wait.h>

static std::atomic<bool> running(true);

void handle_sig(int) {
    running.store(false);
}

bool pid_is_running(pid_t pid) {
    if (pid <= 0) return false;
    return (kill(pid, 0) == 0 || errno == EPERM);
}

bool write_pidfile(const std::string &pidfile, pid_t pid) {
    std::ofstream f(pidfile.c_str());
    if (!f) return false;
    f << pid << std::endl;
    f.close();
    return true;
}

pid_t read_pidfile(const std::string &pidfile) {
    std::ifstream f(pidfile.c_str());
    if (!f) return -1;
    pid_t pid;
    f >> pid;
    if (!f) return -1;
    return pid;
}

void remove_pidfile(const std::string &pidfile) {
    unlink(pidfile.c_str());
}

void daemonize() {
    // First fork
    pid_t pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) {
        // parent exits
        _exit(0);
    }

    // Child continues
    if (setsid() < 0) exit(1);

    // Second fork to prevent reacquisition of a TTY
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) _exit(0);

    // Now in daemon
    umask(0);
    chdir("/");

    // Close standard fds and reopen to /dev/null
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
}

std::string now_str() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

int do_start(bool go_daemon, const std::string &pidfile, const std::string &logpath) {
    // If pidfile exists and process is running, refuse to start
    pid_t existing = read_pidfile(pidfile);
    if (existing > 0 && pid_is_running(existing)) {
        std::cerr << "Already running (pid=" << existing << ")\n";
        return 1;
    }

    if (go_daemon) {
        daemonize();
    }

    // after daemonize (or in foreground) write pidfile
    pid_t mypid = getpid();
    if (!write_pidfile(pidfile, mypid)) {
        std::cerr << "Failed to write pidfile " << pidfile << "\n";
        return 2;
    }
    // install signal handlers
    std::signal(SIGINT, handle_sig);
    std::signal(SIGTERM, handle_sig);

    // open log file (append)
    std::ofstream log(logpath.c_str(), std::ios::app);
    if (!log) {
        // if we cannot open log, continue but print to stderr (if available)
        // but in daemon mode stderr is /dev/null
    }

    // main loop: do trivial work - write timestamp every second
    int counter = 0;
    while (running.load()) {
        std::string entry = now_str() + " bgproc heartbeat " + std::to_string(counter++) + "\n";
        if (log) {
            log << entry;
            log.flush();
        } else {
            // fallback
            std::cerr << entry;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // cleanup
    if (log) {
        log << now_str() << " bgproc shutting down\n";
        log.close();
    }
    remove_pidfile(pidfile);
    return 0;
}

int do_stop(const std::string &pidfile) {
    pid_t pid = read_pidfile(pidfile);
    if (pid <= 0) {
        std::cerr << "No pidfile or invalid pid\n";
        return 1;
    }
    if (!pid_is_running(pid)) {
        std::cerr << "Process not running (pid=" << pid << ") -- removing stale pidfile\n";
        remove_pidfile(pidfile);
        return 1;
    }
    if (kill(pid, SIGTERM) != 0) {
        perror("kill");
        return 2;
    }
    // wait for process to exit (timeout)
    for (int i = 0; i < 20; ++i) {
        if (!pid_is_running(pid)) {
            std::cout << "Stopped " << pid << "\n";
            remove_pidfile(pidfile);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cerr << "Timed out waiting for process to exit\n";
    return 3;
}

int do_status(const std::string &pidfile) {
    pid_t pid = read_pidfile(pidfile);
    if (pid <= 0) {
        std::cout << "Not running (no pidfile)\n";
        return 1;
    }
    if (pid_is_running(pid)) {
        std::cout << "Running pid=" << pid << "\n";
        return 0;
    } else {
        std::cout << "Not running (stale pidfile pid=" << pid << ")\n";
        return 1;
    }
}

void print_usage(const char *prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " start [--foreground] [--pid-file PATH] [--log PATH]\n"
              << "  " << prog << " stop  [--pid-file PATH]\n"
              << "  " << prog << " status [--pid-file PATH]\n"
              << "Defaults:\n"
              << "  pidfile = /tmp/bgproc.pid\n"
              << "  log     = /tmp/bgproc.log\n";
}

int main(int argc, char **argv) {
    std::string pidfile = "/tmp/bgproc.pid";
    std::string logpath = "/tmp/bgproc.log";
    bool foreground = false;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--foreground" || a == "-f") foreground = true;
        else if (a == "--pid-file" && i+1 < argc) { pidfile = argv[++i]; }
        else if (a == "--log" && i+1 < argc) { logpath = argv[++i]; }
        else {
            std::cerr << "Unknown option: " << a << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cmd == "start") {
        return do_start(!foreground, pidfile, logpath);
    } else if (cmd == "stop") {
        return do_stop(pidfile);
    } else if (cmd == "status") {
        return do_status(pidfile);
    } else {
        print_usage(argv[0]);
        return 1;
    }
}
