#pragma once

// OpenBambuFileSystem — FTPS-based replacement for PrinterFileSystem
//
// Provides file listing, download, and delete operations via implicit FTPS
// (port 990) instead of the proprietary BambuTunnel protocol. Used in
// OpenBambu (LAN-only) mode when the BBL networking DLL is not loaded.

#include "slic3r/GUI/Printer/PrinterFileSystem.h"
#include "OpenBambuFtp.hpp"
#include <mutex>
#include <atomic>
#include <thread>

namespace Slic3r { namespace GUI {

class OpenBambuFileSystem : public PrinterFileSystem {
public:
    OpenBambuFileSystem();
    ~OpenBambuFileSystem() override;

    void Attached() override;
    void Start() override;
    void Stop(bool quit = false) override;
    void Retry() override;
    void SetUrl(std::string const &url) override;
    void SetFileType(FileType type, std::string const &storage) override;
    void ListAllFiles() override;
    void DeleteFiles(size_t index) override;
    void DownloadFiles(size_t index, std::string const &path) override;

private:
    std::string m_host;
    std::string m_access_code;
    std::mutex m_ftp_mutex;
    OpenBambu::TlsConn m_ctrl;
    std::atomic<bool> m_stop_flag{false};
    std::thread m_worker;

    bool EnsureConnected();
    std::string GetFtpDirectory() const;
    static bool ParseListLine(const std::string &line,
                              std::string &name, uint64_t &size, time_t &mtime,
                              bool &is_dir);
};

}} // namespace Slic3r::GUI
