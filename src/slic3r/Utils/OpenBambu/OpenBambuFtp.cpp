#include "OpenBambuFtp.hpp"

// Direct FTPS implementation using OpenSSL — no libcurl dependency.
// FTP protocol is simple enough to implement directly for upload:
//   1. TLS connect to port 990 (implicit FTPS)
//   2. USER bblp / PASS access_code
//   3. TYPE I (binary mode)
//   4. PASV (get data port)
//   5. TLS connect to data port
//   6. STOR remote_path
//   7. Send file data on data connection
//   8. Close data connection, read STOR response

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

// Read a line from a TLS connection (up to \r\n)
static std::string tls_readline(SSL *ssl)
{
    std::string line;
    char c;
    while (SSL_read(ssl, &c, 1) == 1) {
        line += c;
        if (c == '\n') break;
    }
    return line;
}

// Read FTP response (may be multi-line). Returns the 3-digit code.
static int ftp_read_response(SSL *ssl, std::string &full_response)
{
    full_response.clear();
    while (true) {
        std::string line = tls_readline(ssl);
        if (line.empty()) return -1;
        full_response += line;

        // FTP responses: "NNN text\r\n" for final line, "NNN-text\r\n" for continuation
        if (line.size() >= 4 && line[3] == ' ') {
            return std::atoi(line.substr(0, 3).c_str());
        }
        // Multi-line: keep reading until we get "NNN " (space after code)
    }
}

static bool ftp_send_cmd(SSL *ssl, const std::string &cmd)
{
    std::string full = cmd + "\r\n";
    return SSL_write(ssl, full.c_str(), (int)full.size()) > 0;
}

// Parse PASV response to extract port: "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
static int parse_pasv_port(const std::string &response)
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

// Create a TLS-wrapped TCP connection
struct TlsConn {
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    int sock = -1;

    // Connect with TLS. If reuse_ctx is provided, reuse that SSL_CTX
    // and copy the session from reuse_ssl for session reuse.
    bool connect(const std::string &host, int port,
                 SSL_CTX *reuse_ctx = nullptr, SSL *reuse_ssl = nullptr)
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
        (void)conn_result;
        if (conn_result < 0) {
#ifdef _WIN32
            fprintf(stderr, "FTP: TCP connect failed (WSA error %d)\n", WSAGetLastError());
#endif
            closesocket(sock); sock = -1;
            return false;
        }

        if (reuse_ctx) {
            ctx = reuse_ctx;
            owns_ctx = false; // borrowed, don't free
            ssl = SSL_new(reuse_ctx);
        } else {
            owns_ctx = true;
            ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
            // Enable session caching for FTPS data channel reuse
            SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
            ssl = SSL_new(ctx);
        }

        SSL_set_fd(ssl, sock);

        // Copy TLS session for session reuse (some FTPS servers require this)
        if (reuse_ssl) {
            SSL_SESSION *sess = SSL_get1_session(reuse_ssl);
            if (sess) {
                SSL_set_session(ssl, sess);
                SSL_SESSION_free(sess);
            }
        }

        // TLS handshake
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
        // TLS connected
        return true;
    }

    bool owns_ctx = true;

    void close()
    {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        if (ctx && owns_ctx) { SSL_CTX_free(ctx); }
        ctx = nullptr;
        if (sock >= 0) { closesocket(sock); sock = -1; }
    }
};

bool FtpUpload::upload(const std::string &host,
                        const std::string &access_code,
                        const std::string &local_path,
                        const std::string &remote_path,
                        OnProgress progress)
{
    // Read the file into memory
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs) {
        fprintf(stderr, "FTP: cannot open %s\n", local_path.c_str());
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    return upload_buffer(host, access_code, data.data(), data.size(), remote_path, progress);
}

bool FtpUpload::upload_buffer(const std::string &host,
                               const std::string &access_code,
                               const void *data, size_t size,
                               const std::string &remote_path,
                               OnProgress progress)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SSL_library_init();

    // Connect control channel (implicit TLS on port 990)
    TlsConn ctrl;
    if (!ctrl.connect(host, 990)) {
        fprintf(stderr, "FTP: failed to connect to %s:990\n", host.c_str());
        return false;
    }

    std::string resp;
    int code;

    // Read server greeting
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 220) {
        fprintf(stderr, "FTP: unexpected greeting: %d %s", code, resp.c_str());
        ctrl.close();
        return false;
    }

    // USER
    ftp_send_cmd(ctrl.ssl, "USER bblp");
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 331 && code != 230) {
        fprintf(stderr, "FTP: USER failed: %d %s", code, resp.c_str());
        ctrl.close();
        return false;
    }

    // PASS
    if (code == 331) {
        ftp_send_cmd(ctrl.ssl, "PASS " + access_code);
        code = ftp_read_response(ctrl.ssl, resp);
        if (code != 230) {
            fprintf(stderr, "FTP: PASS failed: %d %s", code, resp.c_str());
            ctrl.close();
            return false;
        }
    }

    // TYPE I (binary)
    ftp_send_cmd(ctrl.ssl, "TYPE I");
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 200) {
        fprintf(stderr, "FTP: TYPE I failed: %d %s", code, resp.c_str());
        ctrl.close();
        return false;
    }

    // PBSZ 0 + PROT P — enable protection on data channel (required for FTPS)
    ftp_send_cmd(ctrl.ssl, "PBSZ 0");
    code = ftp_read_response(ctrl.ssl, resp);
    // 200 OK expected, but don't fail on error

    ftp_send_cmd(ctrl.ssl, "PROT P");
    code = ftp_read_response(ctrl.ssl, resp);
    // 200 OK expected

    // PASV — get data port
    ftp_send_cmd(ctrl.ssl, "PASV");
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 227) {
        fprintf(stderr, "FTP: PASV failed: %d %s", code, resp.c_str());
        ctrl.close();
        return false;
    }

    int data_port = parse_pasv_port(resp);
    if (data_port < 0) {
        fprintf(stderr, "FTP: failed to parse PASV port from: %s", resp.c_str());
        ctrl.close();
        return false;
    }
    // Data port from PASV

    // STOR command must be sent BEFORE connecting data channel on some servers,
    // but standard FTP says connect data first, then STOR.
    // Try standard order first: connect data, then STOR.

    // STOR command first — some servers only open the passive port after STOR
    ftp_send_cmd(ctrl.ssl, "STOR " + remote_path);

    // NOW open data connection (TLS with session reuse from control channel)
    TlsConn data_conn;
    if (!data_conn.connect(host, data_port, ctrl.ctx, ctrl.ssl)) {
        fprintf(stderr, "FTP: failed to connect data channel on port %d\n", data_port);
        ctrl.close();
        return false;
    }

    // Read STOR response (should be 150 "Opening data connection")
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 150 && code != 125) {
        fprintf(stderr, "FTP: STOR failed: %d %s", code, resp.c_str());
        data_conn.close();
        ctrl.close();
        return false;
    }

    // Send file data over the data connection
    const uint8_t *buf = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < size) {
        int chunk = (int)((size - sent) > 65536 ? 65536 : (size - sent));
        int w = SSL_write(data_conn.ssl, buf + sent, chunk);
        if (w <= 0) {
            fprintf(stderr, "FTP: data write failed at %zu/%zu\n", sent, size);
            data_conn.close();
            ctrl.close();
            return false;
        }
        sent += w;

        if (progress) {
            if (!progress(sent, size)) {
                fprintf(stderr, "FTP: upload cancelled\n");
                data_conn.close();
                ctrl.close();
                return false;
            }
        }
    }

    // Close data connection to signal end of transfer
    data_conn.close();

    // Read STOR completion response
    code = ftp_read_response(ctrl.ssl, resp);
    if (code != 226) {
        fprintf(stderr, "FTP: STOR completion failed: %d %s", code, resp.c_str());
        ctrl.close();
        return false;
    }

    // QUIT
    ftp_send_cmd(ctrl.ssl, "QUIT");
    ftp_read_response(ctrl.ssl, resp); // ignore response
    ctrl.close();

    return true;
}

} // namespace OpenBambu
