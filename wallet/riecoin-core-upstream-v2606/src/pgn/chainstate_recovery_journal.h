#pragma once

#include <util/fs_helpers.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pgn::core {

namespace recovery_fs = std::filesystem;

class ChainstateRecoveryJournal
{
    using path = std::filesystem::path;
    using type = std::filesystem::file_type;

    static constexpr std::array<const char*, 3> GROUPS{
        "blocks/index", "chainstate", "chainstate-pgn-request-history",
    };

    static type Inspect(const path& location)
    {
        std::error_code error;
        const auto status = recovery_fs::symlink_status(location, error);
        if (error == std::errc::no_such_file_or_directory) return type::not_found;
        if (error) throw recovery_fs::filesystem_error("Inspect recovery path", location, error);
        if (status.type() == type::not_found) return type::not_found;
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(location.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            throw std::runtime_error("Recovery path is unavailable or redirects elsewhere");
        }
#endif
        if (status.type() != type::directory && status.type() != type::regular) {
            throw std::runtime_error("Recovery path is not an ordinary file or directory");
        }
        return status.type();
    }

    void CheckRecoveryCapacity() const
    {
        uint64_t bytes{0};
        for (const auto* group : GROUPS) {
            const auto source = m_node / group;
            if (Inspect(source) == type::not_found) continue;
            if (Inspect(source) != type::directory) {
                throw std::runtime_error("Recovery expected a derived database directory");
            }
            for (const auto& entry : recovery_fs::recursive_directory_iterator(source)) {
                if (Inspect(entry.path()) != type::regular) continue;
                const auto length = recovery_fs::file_size(entry.path());
                if (length > std::numeric_limits<uint64_t>::max() / 2 - bytes) {
                    throw recovery_fs::filesystem_error("Disk space is too low for chainstate recovery", std::make_error_code(std::errc::no_space_on_device));
                }
                bytes += length;
            }
        }
        if (!CheckDiskSpace(fs::path(m_node), 2 * bytes)) {
            throw recovery_fs::filesystem_error("Disk space is too low to save and rebuild the chainstate", std::make_error_code(std::errc::no_space_on_device));
        }
    }

    static void CopyDirectory(const path& source, const path& destination)
    {
        if (Inspect(source) != type::directory || !recovery_fs::create_directory(destination)) {
            throw std::runtime_error("Could not stage the recovery directory");
        }
        for (const auto& entry : recovery_fs::directory_iterator(source)) {
            const auto copy_to = destination / entry.path().filename();
            if (Inspect(entry.path()) == type::directory) {
                CopyDirectory(entry.path(), copy_to);
            } else {
                if (!CheckDiskSpace(fs::path(destination), recovery_fs::file_size(entry.path()))) {
                    throw recovery_fs::filesystem_error("Disk space is too low to finish saving the recovery snapshot", std::make_error_code(std::errc::no_space_on_device));
                }
                recovery_fs::copy_file(entry.path(), copy_to);
                FlushFile(copy_to);
            }
        }
    }

    static void FlushFile(const path& location)
    {
        std::unique_ptr<FILE, decltype(&std::fclose)> file{
            fsbridge::fopen(fs::path(location), "rb+"), std::fclose};
        if (!file || !FileCommit(file.get())) throw std::runtime_error("Could not flush the recovery snapshot");
    }

    static void Marker(const path& location, std::string_view body)
    {
        std::ofstream file(location, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        if (!body.empty()) file.write(body.data(), body.size());
        file.close();
        FlushFile(location);
    }

    void Archive(const path& source, const std::string& disposition) const
    {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        const auto archive = m_home / (disposition + "-" + std::to_string(stamp));
        if (Inspect(archive) != type::not_found) throw std::runtime_error("Recovery archive already exists");
        recovery_fs::rename(source, archive);
    }

    static const char* GroupName(size_t index)
    {
        constexpr std::array<const char*, 3> NAMES{"index", "coins", "requests"};
        return NAMES.at(index);
    }

    path m_node;
    path m_home;
    path m_active;

public:
    explicit ChainstateRecoveryJournal(path node)
        : m_node(std::move(node)), m_home(m_node / "pgn-chainstate-recovery"),
          m_active(m_home / "active")
    {
        if (Inspect(m_node) != type::directory || Inspect(m_node / "blocks") != type::directory) {
            throw std::runtime_error("Core data directory or block directory unavailable");
        }
    }

    bool Pending() const
    {
        if (Inspect(m_home) == type::not_found) return false;
        return Inspect(m_active) == type::directory;
    }

    // The caller holds Core's data-directory and block-directory locks throughout.
    void Prepare(std::string_view verified_tip)
    {
        if (verified_tip.size() != 64 || verified_tip.find_first_not_of("0123456789abcdef") != verified_tip.npos) {
            throw std::runtime_error("Recovery requires a verified tip");
        }
        if (Pending()) throw std::runtime_error("A recovery transaction is already pending");
        CheckRecoveryCapacity();
        if (Inspect(m_home) == type::not_found) recovery_fs::create_directory(m_home);
        const auto preparing = m_home / "preparing";
        if (Inspect(preparing) != type::not_found) Archive(preparing, "incomplete");
        if (!recovery_fs::create_directory(preparing)) {
            throw std::runtime_error("Could not create the recovery snapshot");
        }
        for (size_t i{0}; i != GROUPS.size(); ++i) {
            const auto original = m_node / GROUPS[i];
            const auto name = GroupName(i);
            if (Inspect(original) == type::not_found) {
                Marker(preparing / (std::string(name) + ".absent"), "");
            } else {
                CopyDirectory(original, preparing / name);
            }
        }
        Marker(preparing / "tip", verified_tip);
        recovery_fs::rename(preparing, m_active);
    }

    void Restore()
    {
        if (!Pending() || Inspect(m_active / "tip") != type::regular) {
            throw std::runtime_error("Recovery snapshot unavailable");
        }
        const auto rejected = m_active / "discarded";
        if (Inspect(rejected) == type::not_found) recovery_fs::create_directory(rejected);
        for (size_t i{0}; i != GROUPS.size(); ++i) {
            const auto target = m_node / GROUPS[i];
            const auto name = GroupName(i);
            const auto saved = m_active / name;
            const auto replaced = rejected / name;
            const bool existed = Inspect(m_active / (std::string(name) + ".absent")) == type::not_found;
            const auto current = Inspect(target);
            const auto backup = Inspect(saved);
            if (existed && backup == type::not_found) {
                if (current != type::directory) throw std::runtime_error("Recovery snapshot is missing a directory");
                continue;
            }
            if ((existed && backup != type::directory) || (!existed && backup != type::not_found)) {
                throw std::runtime_error("Unexpected object in the recovery snapshot");
            }
            if (current != type::not_found) {
                if (current != type::directory || Inspect(replaced) != type::not_found) {
                    throw std::runtime_error("Recovery cannot move an unexpected replacement");
                }
                recovery_fs::rename(target, replaced);
            }
            if (existed) recovery_fs::rename(saved, target);
        }
        Archive(m_active, "restored");
    }

    bool Complete(std::string_view actual_tip)
    {
        if (!Pending() || Inspect(m_active / "tip") != type::regular) return false;
        std::ifstream file(m_active / "tip", std::ios::binary);
        std::array<char, 65> bytes{};
        file.read(bytes.data(), bytes.size());
        if (file.gcount() != 64 || actual_tip != std::string_view(bytes.data(), 64)) return false;
        file.close();
        Archive(m_active, "verified");
        return true;
    }
};

} // namespace pgn::core
