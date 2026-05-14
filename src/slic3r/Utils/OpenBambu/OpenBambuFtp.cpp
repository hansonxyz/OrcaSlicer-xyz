#include "OpenBambuFtp.hpp"

// Direct FTPS implementation using OpenSSL — no libcurl dependency.
// FTP protocol is simple enough to implement directly for all operations.

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  define closesocket close
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

namespace OpenBambu {

// ============================================================================
// TLS connection
// ============================================================================

bool TlsConn::connect(const std::string &host, int port,
                       SSL_CTX *reuse_ctx, SSL *reuse_ssl)
{
    sock = (int)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

#ifdef _WIN32
    DWORD timeout_ms = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#endif

    int conn_result = ::connect(sock, (sockaddr *)&addr, sizeof(addr));
    if (conn_result < 0) {
#ifdef _WIN32
        fprintf(stderr, "FTP: TCP connect failed (WSA error %d)\n", WSAGetLastError());
#endif
        closesocket(sock); sock = -1;
        return false;
    }

    if (reuse_ctx) {
        ctx = reuse_ctx;
        owns_ctx = false;
        ssl = SSL_new(reuse_ctx);
    } else {
        owns_ctx = true;
        ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
        ssl = SSL_new(ctx);
    }

    SSL_set_fd(ssl, sock);

    if (reuse_ssl) {
        SSL_SESSION *sess = SSL_get1_session(reuse_ssl);
        if (sess) {
            SSL_set_session(ssl, sess);
            SSL_SESSION_free(sess);
        }
    }

    int ssl_ret = SSL_connect(ssl);
    if (ssl_ret != 1) {
        int ssl_err = SSL_get_error(ssl, ssl_ret);
        fprintf(stderr, "FTP: TLS handshake failed, SSL_connect=%d, SSL_get_error=%d\n", ssl_ret, ssl_err);
        unsigned long err;
        while ((err = ERR_get_error()) != 0) {
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            fprintf(stderr, "FTP: OpenSSL: %s\n", buf);
        }
        close();
        return false;
    }
    return true;
}

void TlsConn::close()
{
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
    if (ctx && owns_ctx) { SSL_CTX_free(ctx); }
    ctx = nullptr;
    if (sock >= 0) { closesocket(sock); sock = -1; }
}

// ============================================================================
// FTP protocol helpers
// ============================================================================

std::string ftp_readline(SSL *ssl)
{
    std::string line;
    char c;
    while (SSL_read(ssl, &c, 1) == 1) {
        line += c;
        if (c == '\n') break;
    }
    return line;
}

int ftp_read_response(SSL *ssl, std::string &full_response)
{
    full_response.clear();
    while (true) {
        std::string line = ftp_readline(ssl);
        if (line.empty()) return -1;
        full_response += line;
        if (line.size() >= 4 && line[3] == ' ') {
            return std::atoi(line.substr(0, 3).c_str());
        }
    }
}

bool ftp_send_cmd(SSL *ssl, const std::string &cmd)
{
    std::string full = cmd + "\r\n";
    return SSL_write(ssl, full.c_str(), (int)full.size()) > 0;
}

int parse_pasv_port(const std::string &response)
{
    auto paren = response.find('(');
    if (paren == std::string::npos) return -1;
    auto end = response.find(')', paren);
    if (end == std::string::npos) return -1;

    std::string nums = response.substr(paren + 1, end - paren - 1);
    int parts[6] = {};
    int n = 0;
    std::istringstream iss(nums);
    std::string token;
    while (std::getline(iss, token, ',') && n < 6) {
        parts[n++] = std::atoi(token.c_str());
    }
    if (n < 6) return -1;
    return parts[4] * 256 + parts[5];
}

// ============================================================================
// FTP high-level operations
// ============================================================================

FtpResult ftp_connect(TlsConn &ctrl, const std::string &host, const std::string &access_code)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SSL_library_init();

    if (!ctrl.connect(host, 990)) {
        fprintf(stderr, "FTP: failed to connect to %s:990\n", host.c_str());
        return FtpResult::NetworkError;
    }

    std::string resp;
    int code;

    // 220 Welcome banner
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 220) { ctrl.close(); return FtpResult::NetworkError; }

    // USER → expect 331 (need password) or 230 (no password required).
    // Anything else, including 530 "Not logged in", is a network/protocol failure here.
    ftp_send_cmd(ctrl.ssl, "USER bblp");
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 331 && code != 230) {
        if (code == 530) {
            fprintf(stderr, "FTP: USER bblp rejected (530)\n");
            ctrl.close();
            return FtpResult::AuthFailed;
        }
        ctrl.close();
        return FtpResult::NetworkError;
    }

    if (code == 331) {
        // PASS → 230 ok, 530 means the access code is wrong.
        ftp_send_cmd(ctrl.ssl, "PASS " + access_code);
        code = ftp_read_response(ctrl.ssl, resp);
        if (code == 530) {
            fprintf(stderr, "FTP: PASS rejected (530) — stale access code?\n");
            ctrl.close();
            return FtpResult::AuthFailed;
        }
        if (code != 230) {
            ctrl.close();
            return FtpResult::NetworkError;
        }
    }

    ftp_send_cmd(ctrl.ssl, "TYPE I");
    code = ftp_read_response(ctrl.ssl, resp);

    ftp_send_cmd(ctrl.ssl, "PBSZ 0");
    ftp_read_response(ctrl.ssl, resp);

    ftp_send_cmd(ctrl.ssl, "PROT P");
    ftp_read_response(ctrl.ssl, resp);

    return FtpResult::Ok;
}

std::vector<std::string> ftp_list(TlsConn &ctrl, const std::string &host,
                                   const std::string &directory)
{
    std::vector<std::string> lines;
    std::string resp;
    int code;

    // CWD to directory
    if (!directory.empty() && directory != "/") {
        ftp_send_cmd(ctrl.ssl, "CWD " + directory);
        code = ftp_read_response(ctrl.ssl, resp);
        if (code != 250) {
            fprintf(stderr, "FTP: CWD %s failed: %d\n", directory.c_str(), code);
            return lines;
        }
    } else {
        ftp_send_cmd(ctrl.ssl, "CWD /");
        ftp_read_response(ctrl.ssl, resp);
    }

    // PASV
    ftp_send_cmd(ctrl.ssl, "PASV");
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 227) return lines;

    int data_port = parse_pasv_port(resp);
    if (data_port < 0) return lines;

    // LIST command first, then data connection
    ftp_send_cmd(ctrl.ssl, "LIST");

    TlsConn data_conn;
    if (!data_conn.connect(host, data_port, ctrl.ctx, ctrl.ssl)) {
        ftp_read_response(ctrl.ssl, resp); // consume LIST response
        return lines;
    }

    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 150 && code != 125) {
        data_conn.close();
        return lines;
    }

    // Read data
    std::string all_data;
    char buf[4096];
    while (true) {
        int n = SSL_read(data_conn.ssl, buf, sizeof(buf));
        if (n <= 0) break;
        all_data.append(buf, n);
    }
    data_conn.close();

    // Read LIST completion
    ftp_read_response(ctrl.ssl, resp);

    // Parse into lines
    std::istringstream iss(all_data);
    std::string line;
    while (std::getline(iss, line)) {
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }

    return lines;
}

bool ftp_download(TlsConn &ctrl, const std::string &host,
                  const std::string &remote_path, const std::string &local_path,
                  FtpProgress progress)
{
    std::string resp;
    int code;

    // Get file size via SIZE command
    ftp_send_cmd(ctrl.ssl, "SIZE " + remote_path);
    code = ftp_read_response(ctrl.ssl, resp);
    size_t total_size = 0;
    if (code == 213) {
        // Response: "213 <size>\r\n"
        auto sp = resp.find(' ');
        if (sp != std::string::npos) total_size = (size_t)std::stoll(resp.substr(sp + 1));
    }

    // PASV
    ftp_send_cmd(ctrl.ssl, "PASV");
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 227) return false;

    int data_port = parse_pasv_port(resp);
    if (data_port < 0) return false;

    // RETR command
    ftp_send_cmd(ctrl.ssl, "RETR " + remote_path);

    TlsConn data_conn;
    if (!data_conn.connect(host, data_port, ctrl.ctx, ctrl.ssl)) {
        ftp_read_response(ctrl.ssl, resp);
        return false;
    }

    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 150 && code != 125) {
        data_conn.close();
        return false;
    }

    // Write to local file
    std::ofstream ofs(local_path, std::ios::binary);
    if (!ofs) {
        data_conn.close();
        ftp_read_response(ctrl.ssl, resp);
        return false;
    }

    size_t received = 0;
    char buf[65536];
    while (true) {
        int n = SSL_read(data_conn.ssl, buf, sizeof(buf));
        if (n <= 0) break;
        ofs.write(buf, n);
        received += n;

        if (progress && !progress(received, total_size)) {
            fprintf(stderr, "FTP: download cancelled\n");
            data_conn.close();
            ofs.close();
            ftp_read_response(ctrl.ssl, resp);
            return false;
        }
    }
    data_conn.close();
    ofs.close();

    code = ftp_read_response(ctrl.ssl, resp);
    return (code == 226);
}

bool ftp_delete(TlsConn &ctrl, const std::string &path)
{
    std::string resp;
    ftp_send_cmd(ctrl.ssl, "DELE " + path);
    int code = ftp_read_response(ctrl.ssl, resp);
    return (code == 250);
}

// ============================================================================
// FTP upload (existing functionality, refactored to use shared helpers)
// ============================================================================

FtpResult FtpUpload::upload(const std::string &host,
                            const std::string &access_code,
                            const std::string &local_path,
                            const std::string &remote_path,
                            OnProgress progress)
{
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs) {
        fprintf(stderr, "FTP: cannot open %s\n", local_path.c_str());
        return FtpResult::NetworkError;
    }
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    return upload_buffer(host, access_code, data.data(), data.size(), remote_path, progress);
}

FtpResult FtpUpload::upload_buffer(const std::string &host,
                                   const std::string &access_code,
                                   const void *data, size_t size,
                                   const std::string &remote_path,
                                   OnProgress progress)
{
    TlsConn ctrl;
    FtpResult conn_rc = ftp_connect(ctrl, host, access_code);
    if (conn_rc != FtpResult::Ok) return conn_rc;

    std::string resp;
    int code;

    // PASV
    ftp_send_cmd(ctrl.ssl, "PASV");
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 227) { ctrl.close(); return FtpResult::NetworkError; }

    int data_port = parse_pasv_port(resp);
    if (data_port < 0) { ctrl.close(); return FtpResult::NetworkError; }

    // STOR command first
    ftp_send_cmd(ctrl.ssl, "STOR " + remote_path);

    TlsConn data_conn;
    if (!data_conn.connect(host, data_port, ctrl.ctx, ctrl.ssl)) {
        fprintf(stderr, "FTP: failed to connect data channel on port %d\n", data_port);
        ctrl.close();
        return FtpResult::NetworkError;
    }

    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 150 && code != 125) {
        data_conn.close();
        ctrl.close();
        return FtpResult::NetworkError;
    }

    const uint8_t *buf = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < size) {
        int chunk = (int)((size - sent) > 65536 ? 65536 : (size - sent));
        int w = SSL_write(data_conn.ssl, buf + sent, chunk);
        if (w <= 0) {
            data_conn.close();
            ctrl.close();
            return FtpResult::NetworkError;
        }
        sent += w;
        if (progress && !progress(sent, size)) {
            data_conn.close();
            ctrl.close();
            return FtpResult::NetworkError;
        }
    }

    data_conn.close();
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 226) { ctrl.close(); return FtpResult::NetworkError; }

    ftp_send_cmd(ctrl.ssl, "QUIT");
    ftp_read_response(ctrl.ssl, resp);
    ctrl.close();

    return FtpResult::Ok;
}

} // namespace OpenBambu
