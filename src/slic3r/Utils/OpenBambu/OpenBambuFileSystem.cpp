#include "OpenBambuFileSystem.hpp"
#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/image.h>
#include <wx/dcmemory.h>
#include <sstream>
#include <ctime>
#include <algorithm>

namespace Slic3r { namespace GUI {

OpenBambuFileSystem::OpenBambuFileSystem()
{
    m_status = Initializing;
}

OpenBambuFileSystem::~OpenBambuFileSystem()
{
    m_stop_flag = true;
    // Detach worker thread — it holds a shared_ptr to us via CallAfter,
    // so it's safe to let it finish on its own. Joining here could deadlock
    // if the thread is waiting on CallAfter while the UI thread is in this destructor.
    if (m_worker.joinable()) m_worker.detach();
    std::lock_guard<std::mutex> lock(m_ftp_mutex);
    m_ctrl.close();
}

void OpenBambuFileSystem::Attached()
{
    // Defer the Initializing event — Attached() is called before MediaFilePanel
    // binds its event handlers. CallAfter ensures the event fires after bindings.
    m_status = Initializing;
    CallAfter([this]() {
        SendChangedEvent(EVT_STATUS_CHANGED, (size_t)Initializing, {}, 0);
    });
}

void OpenBambuFileSystem::Start()
{
    m_stop_flag = false;
    if (!m_host.empty()) {
        m_status = Connecting;
        SendChangedEvent(EVT_STATUS_CHANGED, (size_t)Connecting, {}, 0);
        ListAllFiles();
    }
}

void OpenBambuFileSystem::Stop(bool quit)
{
    m_stop_flag = true;
    if (quit && m_worker.joinable()) m_worker.detach();
    std::lock_guard<std::mutex> lock(m_ftp_mutex);
    m_ctrl.close();
}

void OpenBambuFileSystem::Retry()
{
    std::lock_guard<std::mutex> lock(m_ftp_mutex);
    m_ctrl.close();
    Start();
}

void OpenBambuFileSystem::SetUrl(std::string const &url)
{
    // Parse bambu:///local/IP.?...&passwd=CODE...
    // Extract IP: between "local/" and ".?"
    auto local_pos = url.find("local/");
    if (local_pos == std::string::npos) return;
    auto ip_start = local_pos + 6;
    auto ip_end = url.find(".?", ip_start);
    if (ip_end == std::string::npos) ip_end = url.find("?", ip_start);
    if (ip_end == std::string::npos) return;
    m_host = url.substr(ip_start, ip_end - ip_start);

    // Extract passwd= value
    auto passwd_pos = url.find("passwd=");
    if (passwd_pos != std::string::npos) {
        auto val_start = passwd_pos + 7;
        auto val_end = url.find('&', val_start);
        m_access_code = url.substr(val_start, val_end == std::string::npos ? val_end : val_end - val_start);
    }

    BOOST_LOG_TRIVIAL(warning) << "OpenBambuFileSystem: SetUrl host=" << m_host;

    m_status = Connecting;
    SendChangedEvent(EVT_STATUS_CHANGED, (size_t)Connecting, {}, 0);
    ListAllFiles();
}

void OpenBambuFileSystem::SetFileType(FileType type, std::string const &storage)
{
    if (m_file_type == type && m_file_storage == storage) return;
    m_file_type = type;
    m_file_storage = storage;
    SendChangedEvent(EVT_MODE_CHANGED, (size_t)m_group_mode);
    if (!m_host.empty()) ListAllFiles();
}

bool OpenBambuFileSystem::EnsureConnected()
{
    if (m_ctrl.ssl) return true;
    return OpenBambu::ftp_connect(m_ctrl, m_host, m_access_code);
}

std::string OpenBambuFileSystem::GetFtpDirectory() const
{
    switch (m_file_type) {
    case F_TIMELAPSE: return "/timelapse";
    case F_VIDEO:     return "/ipcam";
    case F_MODEL:
    default:          return "/";
    }
}

bool OpenBambuFileSystem::ParseListLine(const std::string &line,
                                         std::string &name, uint64_t &size, time_t &mtime,
                                         bool &is_dir)
{
    // Parse Unix ls -l format from vsFTPd:
    // -rw-r--r--    1 1000     1000     251914400 Jul 19 05:19 filename.mp4
    // drwxr-xr-x    2 1000     1000        32768 Jul 19 17:49 dirname
    if (line.size() < 10) return false;

    is_dir = (line[0] == 'd');

    // Tokenize: permissions, links, user, group, size, month, day, time_or_year, name...
    std::istringstream iss(line);
    std::string perms, links_s, user, group, size_s, month_s, day_s, time_year;
    iss >> perms >> links_s >> user >> group >> size_s >> month_s >> day_s >> time_year;

    // Rest of line is the filename (may contain spaces)
    std::string rest;
    std::getline(iss, rest);
    // Strip leading whitespace
    auto first_non_space = rest.find_first_not_of(' ');
    if (first_non_space == std::string::npos) return false;
    name = rest.substr(first_non_space);

    // Skip . and ..
    if (name == "." || name == "..") return false;

    // Parse size
    try { size = std::stoull(size_s); } catch (...) { size = 0; }

    // Parse date
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
    int mon = 0;
    for (int i = 0; i < 12; i++) {
        if (month_s == months[i]) { mon = i; break; }
    }

    struct tm tm_val{};
    tm_val.tm_mon = mon;
    try { tm_val.tm_mday = std::stoi(day_s); } catch (...) { tm_val.tm_mday = 1; }

    // time_year is either "HH:MM" (recent) or "YYYY" (old)
    if (time_year.find(':') != std::string::npos) {
        // Recent file: "HH:MM" — assume current year
        time_t now = time(nullptr);
        struct tm *now_tm = localtime(&now);
        tm_val.tm_year = now_tm->tm_year;
        auto colon = time_year.find(':');
        try {
            tm_val.tm_hour = std::stoi(time_year.substr(0, colon));
            tm_val.tm_min = std::stoi(time_year.substr(colon + 1));
        } catch (...) {}
    } else {
        // Old file: year
        try { tm_val.tm_year = std::stoi(time_year) - 1900; } catch (...) { tm_val.tm_year = 2024 - 1900; }
    }

    mtime = mktime(&tm_val);
    return true;
}

void OpenBambuFileSystem::ListAllFiles()
{
    if (m_host.empty()) return;

    // Don't start a new listing if one is running
    if (m_worker.joinable()) {
        m_worker.detach();
    }

    auto self = boost::dynamic_pointer_cast<OpenBambuFileSystem>(shared_from_this());
    std::string dir = GetFtpDirectory();
    std::string host = m_host;
    FileType file_type = m_file_type;

    auto weak_self = boost::weak_ptr<OpenBambuFileSystem>(
        boost::dynamic_pointer_cast<OpenBambuFileSystem>(shared_from_this()));

    m_worker = std::thread([weak_self, dir, host, file_type]() {
        auto self = boost::dynamic_pointer_cast<OpenBambuFileSystem>(weak_self.lock());
        if (!self || self->m_stop_flag) return;

        self->CallAfter([weak_self]() {
            auto s = boost::dynamic_pointer_cast<OpenBambuFileSystem>(weak_self.lock());
            if (!s) return;
            s->m_status = ListSyncing;
            s->SendChangedEvent(EVT_STATUS_CHANGED, (size_t)ListSyncing, {}, 0);
        });

        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(self->m_ftp_mutex);
            if (!self->EnsureConnected()) {
                self->CallAfter([weak_self]() {
                    auto s = boost::dynamic_pointer_cast<OpenBambuFileSystem>(weak_self.lock());
                    if (!s) return;
                    s->m_status = Failed;
                    s->m_last_error = 1;
                    s->SendChangedEvent(EVT_STATUS_CHANGED, (size_t)Failed, "Connection failed", 1);
                });
                return;
            }
            lines = OpenBambu::ftp_list(self->m_ctrl, host, dir);
        }

        if (self->m_stop_flag) return;

        // Parse and filter
        FileList files;
        std::string ext_filter;
        if (file_type == F_MODEL) ext_filter = ".3mf";
        else ext_filter = ".mp4";

        for (auto &line : lines) {
            std::string name;
            uint64_t size;
            time_t mtime;
            bool is_dir;
            if (!ParseListLine(line, name, size, mtime, is_dir)) continue;
            if (is_dir) continue;

            // Extension filter
            if (!ext_filter.empty()) {
                if (name.size() < ext_filter.size()) continue;
                std::string lower_name = name.substr(name.size() - ext_filter.size());
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                if (lower_name != ext_filter) continue;
            }

            File f;
            f.name = name;
            f.path = (dir == "/" ? "" : dir + "/") + name;
            f.size = size;
            f.time = mtime;
            f.flags = 0;
            files.push_back(f);
        }

        // Sort by time descending (matches File::operator<)
        std::sort(files.begin(), files.end());

        // Update on UI thread
        self->CallAfter([weak_self, files]() {
            auto self = boost::dynamic_pointer_cast<OpenBambuFileSystem>(weak_self.lock());
            if (!self || self->m_stop_flag) return;

            // Create a placeholder thumbnail for files without one
            wxBitmap placeholder(256, 256);
            {
                wxMemoryDC dc(placeholder);
                dc.SetBackground(wxBrush(wxColour(240, 240, 240)));
                dc.Clear();
                dc.SetPen(*wxLIGHT_GREY_PEN);
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(0, 0, 256, 256);
                // Draw a simple file icon in the center
                dc.SetPen(wxPen(wxColour(180, 180, 180), 2));
                dc.SetBrush(wxBrush(wxColour(220, 220, 220)));
                dc.DrawRoundedRectangle(88, 60, 80, 100, 5);
                // Fold corner
                wxPoint fold[] = { {148, 60}, {168, 80}, {148, 80} };
                dc.DrawPolygon(3, fold);
                // Lines representing text
                dc.SetPen(wxPen(wxColour(190, 190, 190), 2));
                dc.DrawLine(104, 100, 152, 100);
                dc.DrawLine(104, 115, 152, 115);
                dc.DrawLine(104, 130, 140, 130);
            }

            auto file_list = files;
            for (auto &f : file_list) {
                f.thumbnail = placeholder;
            }

            self->m_file_list = file_list;
            self->BuildGroups();
            self->UpdateGroupSelect();
            self->m_status = ListReady;
            self->SendChangedEvent(EVT_FILE_CHANGED, file_list.size());
            self->SendChangedEvent(EVT_STATUS_CHANGED, (size_t)ListReady, {}, 0);
        });
    });
}

void OpenBambuFileSystem::DeleteFiles(size_t index)
{
    // Collect files to delete
    std::vector<std::string> paths;
    std::vector<size_t> indices;

    if (index == (size_t)-1) {
        // Delete all selected
        for (size_t i = 0; i < m_file_list.size(); i++) {
            if (m_file_list[i].IsSelect()) {
                paths.push_back(m_file_list[i].path);
                indices.push_back(i);
            }
        }
    } else if (index < m_file_list.size()) {
        paths.push_back(m_file_list[index].path);
        indices.push_back(index);
    }

    if (paths.empty()) return;

    if (m_worker.joinable()) m_worker.detach();

    auto self = boost::dynamic_pointer_cast<OpenBambuFileSystem>(shared_from_this());
    std::string host = m_host;

    m_worker = std::thread([self, paths, indices, host]() {
        std::vector<size_t> deleted;
        {
            std::lock_guard<std::mutex> lock(self->m_ftp_mutex);
            if (!self->EnsureConnected()) return;

            for (size_t i = 0; i < paths.size(); i++) {
                if (self->m_stop_flag) break;
                if (OpenBambu::ftp_delete(self->m_ctrl, paths[i])) {
                    deleted.push_back(indices[i]);
                    BOOST_LOG_TRIVIAL(warning) << "OpenBambuFileSystem: deleted " << paths[i];
                } else {
                    BOOST_LOG_TRIVIAL(error) << "OpenBambuFileSystem: failed to delete " << paths[i];
                }
            }
        }

        self->CallAfter([self, deleted]() {
            // Remove deleted files from list (reverse order to keep indices valid)
            auto sorted = deleted;
            std::sort(sorted.rbegin(), sorted.rend());
            for (auto idx : sorted) {
                if (idx < self->m_file_list.size())
                    self->m_file_list.erase(self->m_file_list.begin() + idx);
            }
            self->BuildGroups();
            self->UpdateGroupSelect();
            self->SendChangedEvent(EVT_FILE_CHANGED, self->m_file_list.size());
            self->SendChangedEvent(EVT_SELECT_CHANGED, 0);
        });
    });
}

void OpenBambuFileSystem::DownloadFiles(size_t index, std::string const &path)
{
    // Collect files to download
    struct DownloadItem { size_t index; std::string remote; std::string local; };
    std::vector<DownloadItem> items;

    auto make_local = [&path](const std::string &name) {
        return path + "/" + name;
    };

    if (index == (size_t)-1) {
        for (size_t i = 0; i < m_file_list.size(); i++) {
            if (m_file_list[i].IsSelect()) {
                items.push_back({i, m_file_list[i].path, make_local(m_file_list[i].name)});
            }
        }
    } else if (index < m_file_list.size()) {
        items.push_back({index, m_file_list[index].path, make_local(m_file_list[index].name)});
    }

    if (items.empty()) return;

    if (m_worker.joinable()) m_worker.detach();

    auto self = boost::dynamic_pointer_cast<OpenBambuFileSystem>(shared_from_this());
    std::string host = m_host;

    m_worker = std::thread([self, items, host]() {
        std::lock_guard<std::mutex> lock(self->m_ftp_mutex);
        if (!self->EnsureConnected()) return;

        for (auto &item : items) {
            if (self->m_stop_flag) break;

            bool ok = OpenBambu::ftp_download(self->m_ctrl, host,
                item.remote, item.local, [self, &item](size_t dl, size_t total) {
                    // Progress callback — could post EVT_DOWNLOAD with progress
                    return !self->m_stop_flag.load();
                });

            int result = ok ? 0 : 1;
            std::string msg = ok ? item.local : "Download failed";

            self->CallAfter([self, idx = item.index, result, msg]() {
                self->SendChangedEvent(EVT_DOWNLOAD, idx, msg, result);
            });

            if (ok) {
                BOOST_LOG_TRIVIAL(warning) << "OpenBambuFileSystem: downloaded " << item.remote << " -> " << item.local;
            } else {
                BOOST_LOG_TRIVIAL(error) << "OpenBambuFileSystem: download failed " << item.remote;
            }
        }
    });
}

}} // namespace Slic3r::GUI
