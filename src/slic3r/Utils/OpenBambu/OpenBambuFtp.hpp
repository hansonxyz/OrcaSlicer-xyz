#pragma once

// OpenBambu FTPS operations
//
// Communicates with Bambu printers via implicit FTPS on port 990.
// Authentication: username "bblp", password = LAN access code.
// TLS: implicit (port 990), self-signed cert (skip verification).
// Passive mode required, passive ports 50000-50100.

#include <string>
#include <vector>
#include <functional>

// Forward declarations for OpenSSL types (avoid including headers)
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace OpenBambu {

// ============================================================================
// TLS connection helper (shared by all FTP operations)
// ============================================================================
struct TlsConn {
    SSL_CTX *ctx = nullptr;
    SSL     *ssl = nullptr;
    int      sock = -1;
    bool     owns_ctx = false;

    // Connect with fresh TLS context, or reuse an existing context/session
    // for data channel connections (required by vsFTPd for session reuse).
    bool connect(const std::string &host, int port,
                 SSL_CTX *reuse_ctx = nullptr, SSL *reuse_ssl = nullptr);
    void close();
};

// ============================================================================
// FTP protocol helpers
// ============================================================================
std::string ftp_readline(SSL *ssl);
int         ftp_read_response(SSL *ssl, std::string &full_response);
bool        ftp_send_cmd(SSL *ssl, const std::string &cmd);
int         parse_pasv_port(const std::string &response);

// ============================================================================
// FTP high-level operations
// ============================================================================

// Establish an authenticated FTPS control connection.
// Returns true if ctrl is connected, authenticated, and PROT P set.
bool ftp_connect(TlsConn &ctrl, const std::string &host, const std::string &access_code);

// List directory contents. Returns raw LIST output lines.
std::vector<std::string> ftp_list(TlsConn &ctrl, const std::string &host,
                                   const std::string &directory);

// Download a remote file to a local path.
// Progress callback: (bytes_downloaded, bytes_total) -> return false to cancel.
using FtpProgress = std::function<bool(size_t downloaded, size_t total)>;
bool ftp_download(TlsConn &ctrl, const std::string &host,
                  const std::string &remote_path, const std::string &local_path,
                  FtpProgress progress = nullptr);

// Delete a remote file.
bool ftp_delete(TlsConn &ctrl, const std::string &remote_path);

// ============================================================================
// FTP upload (existing functionality)
// ============================================================================
class FtpUpload {
public:
    using OnProgress = std::function<bool(size_t uploaded, size_t total)>;

    static bool upload(const std::string &host,
                       const std::string &access_code,
                       const std::string &local_path,
                       const std::string &remote_path,
                       OnProgress progress = nullptr);

    static bool upload_buffer(const std::string &host,
                              const std::string &access_code,
                              const void *data, size_t size,
                              const std::string &remote_path,
                              OnProgress progress = nullptr);
};

} // namespace OpenBambu
