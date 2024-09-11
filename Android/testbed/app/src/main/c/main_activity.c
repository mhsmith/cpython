#include <android/log.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <Python.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>


static void throw_runtime_exception(JNIEnv *env, const char *message) {
    (*env)->ThrowNew(
        env,
        (*env)->FindClass(env, "java/lang/RuntimeException"),
        message);
}

static void throw_errno(JNIEnv *env, char *error_prefix) {
    char error_message[1024];
    snprintf(error_message, sizeof(error_message),
             "%s: %s", error_prefix, strerror(errno));
    throw_runtime_exception(env, error_message);
}


// --- Stdio redirection ------------------------------------------------------

// Most apps won't need this, because the Python-level sys.stdout and sys.stderr
// are redirected to the Android logcat by Python itself. However, in the
// testbed it's useful to redirect the native streams as well, to debug problems
// in the Python startup or redirection process.
//
// Based on
// https://github.com/beeware/briefcase-android-gradle-template/blob/v0.3.11/%7B%7B%20cookiecutter.safe_formal_name%20%7D%7D/app/src/main/cpp/native-lib.cpp

typedef struct {
    FILE *file;
    int fd;
    android_LogPriority priority;
    char *tag;
    int pipe[2];
} StreamInfo;

static StreamInfo STREAMS[] = {
    {stdout, STDOUT_FILENO, ANDROID_LOG_INFO, "native.stdout", {-1, -1}},
    {stderr, STDERR_FILENO, ANDROID_LOG_WARN, "native.stderr", {-1, -1}},
    {NULL, -1, ANDROID_LOG_UNKNOWN, NULL, {-1, -1}},
};

// The maximum length of a log message in bytes, including the level marker and
// tag, is defined as LOGGER_ENTRY_MAX_PAYLOAD in
// platform/system/logging/liblog/include/log/log.h. As of API level 30, messages
// longer than this will be be truncated by logcat. This limit has already been
// reduced at least once in the history of Android (from 4076 to 4068 between API
// level 23 and 26), so leave some headroom.
static const int MAX_BYTES_PER_WRITE = 4000;

static void *redirection_thread(void *arg) {
    StreamInfo *si = (StreamInfo*)arg;
    ssize_t read_size;
    char buf[MAX_BYTES_PER_WRITE];
    while ((read_size = read(si->pipe[0], buf, sizeof buf - 1)) > 0) {
        buf[read_size] = '\0'; /* add null-terminator */
        __android_log_write(si->priority, si->tag, buf);
    }
    return 0;
}

static char *redirect_stream(StreamInfo *si) {
    /* make the FILE unbuffered, to ensure messages are never lost */
    if (setvbuf(si->file, 0, _IONBF, 0)) {
        return "setvbuf";
    }

    /* create the pipe and redirect the file descriptor */
    if (pipe(si->pipe)) {
        return "pipe";
    }
    if (dup2(si->pipe[1], si->fd) == -1) {
        return "dup2";
    }

    /* start the logging thread */
    pthread_t thr;
    if ((errno = pthread_create(&thr, 0, redirection_thread, si))) {
        return "pthread_create";
    }
    if ((errno = pthread_detach(thr))) {
        return "pthread_detach";
    }
    return 0;
}

JNIEXPORT void JNICALL Java_org_python_testbed_PythonTestRunner_redirectStdioToLogcat(
    JNIEnv *env, jobject obj
) {
    for (StreamInfo *si = STREAMS; si->file; si++) {
        char *error_prefix;
        if ((error_prefix = redirect_stream(si))) {
            throw_errno(env, error_prefix);
            return;
        }
    }
}


// --- Signal handling ---------------------------------------------------------

JNIEXPORT void JNICALL Java_org_python_testbed_PythonTestRunner_sendSignal(
    JNIEnv *env, jobject obj, int sig
) {
    if (kill(getpid(), sig) != 0) {
        throw_errno(env, "kill");
        return;
    }
}

// This signal handler calls the raw _exit system call, which terminates the
// current thread.
static void exit_handler(int sig) {
    syscall(SYS_exit, 0);
}

// Android doesn't implement pthread_cancel, but we can achieve something
// similar by forcing the thread to run a signal handler.
JNIEXPORT void JNICALL Java_org_python_testbed_PythonTestRunner_killThread(
    JNIEnv *env, jobject obj, int tid
) {
    int sig = SIGUSR2;
    sighandler_t old_handler;
    if ((old_handler = signal(sig, exit_handler)) == SIG_ERR) {
        throw_errno(env, "signal (install)");
        return;
    }
    if (tgkill(getpid(), tid, sig) != 0) {
        throw_errno(env, "tgkill");
        return;
    }

    // After a short delay, verify that the thread has exited.
    usleep(100000);
    if (tgkill(getpid(), tid, sig) == 0) {
        fprintf(
            stderr,
            "SignalCatcher TID %d still exists - signal tests may be unreliable",
            tid
        );
    }

    if (signal(sig, old_handler) == SIG_ERR) {
        throw_errno(env, "signal (uninstall)");
        return;
    }
}

JNIEXPORT void JNICALL Java_org_python_testbed_PythonTestRunner_unblockSignal(
    JNIEnv *env, jobject obj, int sig
) {
    sigset_t sigset;
    if (sigemptyset(&sigset) != 0) {
        throw_errno(env, "sigemptyset");
        return;
    }
    if (sigaddset(&sigset, sig) != 0) {
        throw_errno(env, "sigaddset");
        return;
    }
    if (sigprocmask(SIG_UNBLOCK, &sigset, NULL) != 0) {
        throw_errno(env, "sigprocmask");
        return;
    }
}


// --- Python initialization ---------------------------------------------------

static PyStatus set_config_string(
    JNIEnv *env, PyConfig *config, wchar_t **config_str, jstring value
) {
    const char *value_utf8 = (*env)->GetStringUTFChars(env, value, NULL);
    PyStatus status = PyConfig_SetBytesString(config, config_str, value_utf8);
    (*env)->ReleaseStringUTFChars(env, value, value_utf8);
    return status;
}

static void throw_status(JNIEnv *env, PyStatus status) {
    throw_runtime_exception(env, status.err_msg ? status.err_msg : "");
}

JNIEXPORT int JNICALL Java_org_python_testbed_PythonTestRunner_runPython(
    JNIEnv *env, jobject obj, jstring home, jstring runModule
) {
    PyConfig config;
    PyStatus status;
    PyConfig_InitIsolatedConfig(&config);

    status = set_config_string(env, &config, &config.home, home);
    if (PyStatus_Exception(status)) {
        throw_status(env, status);
        return 1;
    }

    status = set_config_string(env, &config, &config.run_module, runModule);
    if (PyStatus_Exception(status)) {
        throw_status(env, status);
        return 1;
    }

    // Some tests generate SIGPIPE and SIGXFSZ, which should be ignored.
    config.install_signal_handlers = 1;

    status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) {
        throw_status(env, status);
        return 1;
    }

    return Py_RunMain();
}
