#pragma once

// OpenBambu FTPS Upload
//
// Uploads gcode/3mf files to Bambu printers via implicit FTPS on port 990.
// Uses libcurl (already an OrcaSlicer dependency).
//
// Authentication: username "bblp", password = LAN access code.
// TLS: implicit (port 990), self-signed cert (skip verification).
// Passive mode required, passive ports 50000-50100.

#include <string>
#include <functional>

namespace OpenBambu {

class FtpUpload {
public:
    // Progress callback: (bytes_uploaded, bytes_total) -> return false to cancel
    using OnProgress = std::function<bool(size_t uploaded, size_t total)>;

    // Upload a local file to the printer.
    // remote_path: filename on the printer (e.g., "model.3mf" or "cache/model.3mf")
    // Returns true on success.
    static bool upload(const std::string &host,
                       const std::string &access_code,
                       const std::string &local_path,
                       const std::string &remote_path,
                       OnProgress progress = nullptr);

    // Upload from a memory buffer instead of a file on disk.
    static bool upload_buffer(const std::string &host,
                              const std::string &access_code,
                              const void *data, size_t size,
                              const std::string &remote_path,
                              OnProgress progress = nullptr);
};

} // namespace OpenBambu
