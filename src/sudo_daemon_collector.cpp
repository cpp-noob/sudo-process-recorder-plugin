#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

int main(int argc, char** argv)
{
    std::string sock = (argc > 1)
        ? argv[1]
        : "/run/sudo-process-recorder.sock";

    std::string out = (argc > 2)
        ? argv[2]
        : "/var/log/sudo-process-recorder.jsonl";

    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    ::unlink(sock.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock.c_str());

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    for (;;) {
        char buf[65536];
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;

        buf[n] = '\0';

        std::ofstream ofs(out, std::ios::app);
        ofs << buf << "\n";
    }
}
