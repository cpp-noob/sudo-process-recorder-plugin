#include <sudo_plugin.h>

#include <cstring>
#include <cstdarg>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

static sudo_printf_t g_printf = nullptr;
static std::string g_preload_lib_path = "/usr/lib/syscall_interceptor.so";
static std::string g_daemon_sock = "/run/sudo-process-recorder.sock";
static std::string g_log_file = "";
static FILE* g_log_fp = nullptr;

static bool
file_exists(const std::string& path)
{
    struct stat buffer;
    return (::stat(path.c_str(), &buffer) == 0);
}

static const char*
find_kv(char* const list[], const char* key)
{
    if (!list) return "";
    const size_t klen = std::strlen(key);
    for (size_t i = 0; list[i]; i++) {
        if (std::strncmp(list[i], key, klen) == 0 && list[i][klen] == '=') {
            return list[i] + klen + 1;
        }
    }
    return "";
}

static std::string
json_escape(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:   o += c; break;
        }
    }
    return o;
}

static void
send_to_daemon(const std::string& sock_path, const std::string& payload)
{
    if (sock_path.empty()) return;

    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path.c_str());

    (void)::sendto(fd, payload.data(), payload.size(), 0,
                   reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
}

static void
write_log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    
    if (g_log_fp) {
        vfprintf(g_log_fp, format, args);
        fflush(g_log_fp);
    } else if (g_printf) {
        char buffer[4096];
        vsnprintf(buffer, sizeof(buffer), format, args);
        g_printf(SUDO_CONV_INFO_MSG, "%s", buffer);
    }
    
    va_end(args);
}

static void
parse_plugin_options(char* const plugin_options[])
{
    if (!plugin_options) return;
    for (size_t i = 0; plugin_options[i]; i++) {
        const char* s = plugin_options[i];
        const char* eq = std::strchr(s, '=');
        if (!eq) continue;
        std::string k(s, eq);
        std::string v(eq + 1);
        if (k == "daemon_sock") g_daemon_sock = v;
        else if (k == "preload_lib") g_preload_lib_path = v;
        else if (k == "log_file") g_log_file = v;
    }
}

static int
io_open(
    unsigned int version,
    sudo_conv_t conv,
    sudo_printf_t plugin_printf,
    char* const settings[],
    char* const user_info[],
    char* const command_info[],
    int argc,
    char* const argv[],
    char* const user_env[],
    char* const plugin_options[],
    const char **errstr)
{
    (void)version;
    (void)conv;
    (void)argc;
    (void)argv;
    (void)user_env;
    (void)errstr;

    g_printf = plugin_printf;
    parse_plugin_options(plugin_options);

    if (!g_log_file.empty()) {
        g_log_fp = fopen(g_log_file.c_str(), "a");
        if (!g_log_fp && g_printf) {
            g_printf(SUDO_CONV_ERROR_MSG, "Warning: Failed to open log file %s\n", g_log_file.c_str());
        }
    }

    const std::string user       = find_kv(user_info, "user");
    const std::string ppid       = find_kv(user_info, "ppid");
    const std::string pgid       = find_kv(user_info, "pgid");
    const std::string runas_user = find_kv(settings,  "runas_user");
    const std::string runas_uid  = find_kv(settings,  "runas_uid");
    const std::string runas_gid  = find_kv(settings,  "runas_gid");
    const std::string command    = find_kv(command_info, "command");

    const pid_t pid = ::getpid();

    // Check if preload library exists and inject LD_PRELOAD only if available
    bool preload_enabled = false;
    if (!g_preload_lib_path.empty() && file_exists(g_preload_lib_path)) {
        ::setenv("LD_PRELOAD", g_preload_lib_path.c_str(), 1);
        ::setenv("SUDO_DAEMON_SOCK", g_daemon_sock.c_str(), 1);
        if (!g_log_file.empty()) {
            ::setenv("SUDO_LOG_FILE", g_log_file.c_str(), 1);
        } else {
            ::setenv("SUDO_LOG_FILE", "stdout", 1);
        }
        preload_enabled = true;
    }
    
    write_log("\n=== Basic Process Info ===\n");

    write_log(
        "User: %s | Command: %s | PID: %d | PPID: %s\n",
        user.c_str(),
        command.c_str(),
        pid,
        ppid.c_str()
    );

    if (preload_enabled) {
        write_log("Tracking child processes via LD_PRELOAD...\n");
    } else {
        write_log("Note: LD_PRELOAD library not found, tracking without preload\n");
    }
    write_log("==========================\n");

    std::ostringstream os;
    os << "{"
       << "\"basic_info\":{"
       << "\"user\":\""       << json_escape(user) << "\","
       << "\"pid\":"          << pid << ","
       << "\"ppid\":"         << (ppid.empty() ? "null" : ppid) << ","
       << "\"pgid\":"         << (pgid.empty() ? "null" : pgid) << ","
       << "\"runas_user\":\"" << json_escape(runas_user) << "\","
       << "\"runas_uid\":"    << (runas_uid.empty() ? "null" : runas_uid) << ","
       << "\"runas_gid\":"    << (runas_gid.empty() ? "null" : runas_gid) << ","
       << "\"command\":\""    << json_escape(command) << "\""
       << "}"
       << "}";

    const std::string payload = os.str();

    send_to_daemon(g_daemon_sock, payload);

    return 1;
}

static void io_close(int, int) {
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = nullptr;
    }
}

static int passthru(const char*, unsigned int, const char**) { return 1; }

extern "C" {

struct io_plugin sudo_process_recorder = {
    SUDO_IO_PLUGIN,
    SUDO_API_VERSION,
    io_open,
    io_close,
    nullptr,
    passthru,
    passthru,
    passthru,
    passthru,
    passthru,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

}
