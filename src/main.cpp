#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

static std::string read_line(int sockfd) {
    std::string line;
    char c;
    bool last_was_cr = false;
    while (true) {
        ssize_t n = read(sockfd, &c, 1);
        if (n <= 0)
            break;
        if (c == '\r') {
            last_was_cr = true;
            continue;
        }
        if (c == '\n') {
            break;
        }
        if (last_was_cr) {
            line.push_back('\r');
        }
        line.push_back(c);
        last_was_cr = false;
    }
    return line;
}

static std::vector<std::string> read_response(int sockfd) {
    std::vector<std::string> lines;
    std::string first_line = read_line(sockfd);
    if (first_line.empty())
        return lines;
    lines.push_back(first_line);

    if (first_line.size() >= 4 && first_line[3] == '-') {
        std::string code = first_line.substr(0, 3);
        // multiline response
        while (true) {
            std::string l = read_line(sockfd);
            if (l.empty())
                break;
            lines.push_back(l);
            if (l.size() >= 4 && l.substr(0, 3) == code && l[3] == ' ')
                break;
        }
    }

    return lines;
}

static void send_cmd(int sockfd, const std::string &cmd) {
    std::string data = cmd + "\r\n";
    write(sockfd, data.c_str(), data.size());
}

static bool parse_pasv_response(const std::string &str, std::string &ip,
                                int &port) {
    size_t start = str.find('(');
    size_t end = str.find(')');
    if (start == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::string inside = str.substr(start + 1, end - start - 1);
    int h1, h2, h3, h4, p1, p2;
    if (sscanf(inside.c_str(), "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1,
               &p2) != 6) {
        return false;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", h1, h2, h3, h4);
    ip = buf;
    port = p1 * 256 + p2;
    return true;
}

static int connect_to_server(const std::string &hostname, int port) {
    struct addrinfo hints {
    }, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(hostname.c_str(), port_str, &hints, &res) != 0)
        return -1;

    int sockfd = -1;
    for (auto p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0)
            continue;
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    return sockfd;
}

class FTPClient {
  public:
    FTPClient() : ctrl_sock(-1), logged_in(false) {}

    ~FTPClient() {
        if (ctrl_sock >= 0) {
            logout();
            close(ctrl_sock);
        }
    }

    bool connect_to_host(const std::string &host, int port = 21) {
        ctrl_sock = connect_to_server(host, port);
        if (ctrl_sock < 0) {
            std::cerr << "Failed to connect to server.\n";
            return false;
        }
        auto lines = read_response(ctrl_sock);
        if (lines.empty() || lines[0].compare(0, 3, "220") != 0) {
            std::cerr << "Server did not send a proper greeting.\n";
            return false;
        }
        connected_host = host;
        return true;
    }

    bool login(const std::string &user, const std::string &pass) {
        if (ctrl_sock < 0) {
            std::cerr << "No control connection.\n";
            return false;
        }
        send_cmd(ctrl_sock, "USER " + user);
        auto lines = read_response(ctrl_sock);
        if (lines.empty() || (lines[0].compare(0, 3, "331") != 0 &&
                              lines[0].compare(0, 3, "230") != 0)) {
            std::cerr << "USER command failed.\n";
            return false;
        }

        if (lines[0].compare(0, 3, "230") != 0) {
            send_cmd(ctrl_sock, "PASS " + pass);
            lines = read_response(ctrl_sock);
            if (lines.empty() || lines[0].compare(0, 3, "230") != 0) {
                std::cerr << "PASS command failed.\n";
                return false;
            }
        }

        logged_in = true;
        return true;
    }

    bool logout() {
        if (ctrl_sock < 0)
            return true;
        send_cmd(ctrl_sock, "QUIT");
        auto lines = read_response(ctrl_sock);
        if (!lines.empty() && lines[0].compare(0, 3, "221") == 0) {
            logged_in = false;
            close(ctrl_sock);
            ctrl_sock = -1;
            return true;
        }
        return false;
    }

    bool list_files() {
        if (!ensure_logged_in())
            return false;
        std::string data_ip;
        int data_port;
        if (!enter_passive_mode(data_ip, data_port))
            return false;

        int data_sock = connect_to_server(data_ip, data_port);
        if (data_sock < 0) {
            std::cerr << "Failed to connect data socket.\n";
            return false;
        }

        send_cmd(ctrl_sock, "LIST");
        auto lines = read_response(ctrl_sock);
        if (lines.empty() || (lines[0].compare(0, 3, "150") != 0 &&
                              lines[0].compare(0, 3, "125") != 0)) {
            std::cerr << "LIST did not get expected opening.\n";
            close(data_sock);
            return false;
        }

        char buffer[1024];
        ssize_t n;
        while ((n = read(data_sock, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[n] = '\0';
            std::cout << buffer;
        }
        close(data_sock);
        lines = read_response(ctrl_sock);
        if (lines.empty() || lines[0].compare(0, 3, "226") != 0) {
            std::cerr << "Did not receive 226 on list completion.\n";
            return false;
        }
        return true;
    }

    bool download_file(const std::string &remote_filename,
                       const std::string &local_filename) {
        if (!ensure_logged_in())
            return false;
        if (!set_type(current_type)) {
            std::cerr << "Failed to set transfer type.\n";
            return false;
        }

        std::string data_ip;
        int data_port;
        if (!enter_passive_mode(data_ip, data_port))
            return false;

        int data_sock = connect_to_server(data_ip, data_port);
        if (data_sock < 0) {
            std::cerr << "Failed to connect data socket.\n";
            return false;
        }

        send_cmd(ctrl_sock, "RETR " + remote_filename);
        auto lines = read_response(ctrl_sock);
        if (lines.empty() || (lines[0].compare(0, 3, "150") != 0 &&
                              lines[0].compare(0, 3, "125") != 0)) {
            std::cerr << "RETR command failed.\n";
            close(data_sock);
            return false;
        }

        FILE *f = fopen(local_filename.c_str(), "wb");
        if (!f) {
            std::cerr << "Failed to open local file for writing.\n";
            close(data_sock);
            return false;
        }

        char buffer[4096];
        ssize_t n;
        while ((n = read(data_sock, buffer, sizeof(buffer))) > 0) {
            fwrite(buffer, 1, n, f);
        }
        fclose(f);
        close(data_sock);

        lines = read_response(ctrl_sock);
        if (lines.empty() || lines[0].compare(0, 3, "226") != 0) {
            std::cerr << "Did not get final 226 on file transfer completion.\n";
            return false;
        }

        return true;
    }

    bool upload_file(const std::string &local_filename,
                     const std::string &remote_filename) {
        if (!ensure_logged_in())
            return false;
        if (!set_type(current_type)) {
            std::cerr << "Failed to set transfer type.\n";
            return false;
        }

        std::string data_ip;
        int data_port;
        if (!enter_passive_mode(data_ip, data_port))
            return false;

        int data_sock = connect_to_server(data_ip, data_port);
        if (data_sock < 0) {
            std::cerr << "Failed to connect data socket.\n";
            return false;
        }

        send_cmd(ctrl_sock, "STOR " + remote_filename);
        auto lines = read_response(ctrl_sock);
        if (lines.empty() || (lines[0].compare(0, 3, "150") != 0 &&
                              lines[0].compare(0, 3, "125") != 0)) {
            std::cerr << "STOR command failed.\n";
            close(data_sock);
            return false;
        }

        FILE *f = fopen(local_filename.c_str(), "rb");
        if (!f) {
            std::cerr << "Failed to open local file for reading.\n";
            close(data_sock);
            return false;
        }

        char buffer[4096];
        size_t nread;
        while ((nread = fread(buffer, 1, sizeof(buffer), f)) > 0) {
            ssize_t nwritten = 0;
            while (nwritten < (ssize_t)nread) {
                ssize_t ret =
                    write(data_sock, buffer + nwritten, nread - nwritten);
                if (ret <= 0) {
                    std::cerr << "Failed to write to data socket.\n";
                    fclose(f);
                    close(data_sock);
                    return false;
                }
                nwritten += ret;
            }
        }
        fclose(f);
        close(data_sock);

        lines = read_response(ctrl_sock);
        if (lines.empty() || lines[0].compare(0, 3, "226") != 0) {
            std::cerr << "Did not get 226 on file upload completion.\n";
            return false;
        }

        return true;
    }

    bool set_type(char t) {
        if (ctrl_sock < 0)
            return false;
        std::string cmd = std::string("TYPE ") + t;
        send_cmd(ctrl_sock, cmd);
        auto lines = read_response(ctrl_sock);
        if (lines.empty() || lines[0].compare(0, 3, "200") != 0) {
            return false;
        }
        current_type = t;
        return true;
    }

    bool is_logged_in() const { return logged_in; }

  private:
    int ctrl_sock;
    bool logged_in;
    std::string connected_host;
    char current_type = 'I'; // 'I' for binary, 'A' for ASCII by default

    bool ensure_logged_in() {
        if (!logged_in) {
            std::cerr << "Not logged in.\n";
            return false;
        }
        return true;
    }

    bool enter_passive_mode(std::string &data_ip, int &data_port) {
        send_cmd(ctrl_sock, "PASV");
        auto lines = read_response(ctrl_sock);
        if (lines.empty() || lines[0].compare(0, 3, "227") != 0) {
            std::cerr << "PASV command failed.\n";
            return false;
        }
        if (!parse_pasv_response(lines[0], data_ip, data_port)) {
            std::cerr << "Failed to parse PASV response.\n";
            return false;
        }
        return true;
    }
};

int main() {
    FTPClient ftp;
    std::string command;

    std::cout
        << "Welcome to the FTP client.\nType 'help' for available commands.\n";

    while (true) {
        std::cout << "ftp> ";
        if (!std::getline(std::cin, command)) {
            break; // EOF
        }

        if (command.empty())
            continue;

        // Parse command
        std::string cmd;
        std::string arg1, arg2;
        {
            // Simple tokenizer
            size_t pos = command.find(' ');
            if (pos == std::string::npos) {
                cmd = command;
            } else {
                cmd = command.substr(0, pos);
                std::string rest = command.substr(pos + 1);
                // find next arg
                pos = rest.find(' ');
                if (pos == std::string::npos) {
                    arg1 = rest;
                } else {
                    arg1 = rest.substr(0, pos);
                    arg2 = rest.substr(pos + 1);
                }
            }
        }

        if (cmd == "help") {
            std::cout << "Available commands:\n";
            std::cout << " connect <host> [port]   - Connect to FTP server\n";
            std::cout << " login <user> <pass>     - Login with username and "
                         "password\n";
            std::cout << " ls                      - List files\n";
            std::cout << " get <remote> [local]    - Download file\n";
            std::cout << " put <local> [remote]    - Upload file\n";
            std::cout << " type [a|i]              - Set transfer type (ASCII "
                         "or binary)\n";
            std::cout << " quit                    - Quit the application\n";
        } else if (cmd == "connect") {
            int port = 21;
            if (!arg1.empty()) {
                if (!arg2.empty()) {
                    port = std::stoi(arg2);
                }
                if (!ftp.connect_to_host(arg1, port)) {
                    std::cerr << "Failed to connect.\n";
                } else {
                    std::cout << "Connected to " << arg1 << std::endl;
                }
            } else {
                std::cerr << "Usage: connect <host> [port]\n";
            }
        } else if (cmd == "login") {
            if (arg1.empty() || arg2.empty()) {
                std::cerr << "Usage: login <user> <pass>\n";
            } else {
                if (ftp.login(arg1, arg2)) {
                    std::cout << "Logged in as " << arg1 << std::endl;
                } else {
                    std::cerr << "Login failed.\n";
                }
            }
        } else if (cmd == "ls") {
            if (!ftp.list_files()) {
                std::cerr << "Failed to list files.\n";
            }
        } else if (cmd == "get") {
            if (arg1.empty()) {
                std::cerr << "Usage: get <remote> [local]\n";
            } else {
                std::string local = arg2.empty() ? arg1 : arg2;
                if (!ftp.download_file(arg1, local)) {
                    std::cerr << "Download failed.\n";
                } else {
                    std::cout << "Downloaded " << arg1 << " to " << local
                              << std::endl;
                }
            }
        } else if (cmd == "put") {
            if (arg1.empty()) {
                std::cerr << "Usage: put <local> [remote]\n";
            } else {
                std::string remote = arg2.empty() ? arg1 : arg2;
                if (!ftp.upload_file(arg1, remote)) {
                    std::cerr << "Upload failed.\n";
                } else {
                    std::cout << "Uploaded " << arg1 << " as " << remote
                              << std::endl;
                }
            }
        } else if (cmd == "type") {
            if (arg1.empty()) {
                std::cout << "Usage: type [a|i]\n";
            } else {
                char t = (arg1[0] == 'a' || arg1[0] == 'A') ? 'A' : 'I';
                if (!ftp.set_type(t)) {
                    std::cerr << "Failed to set type.\n";
                } else {
                    std::cout << "Type set to "
                              << (t == 'A' ? "ASCII" : "Binary") << std::endl;
                }
            }
        } else if (cmd == "quit") {
            std::cout << "Goodbye.\n";
            break;
        } else {
            std::cerr << "Unknown command: " << cmd << "\n";
        }
    }

    return 0;
}
