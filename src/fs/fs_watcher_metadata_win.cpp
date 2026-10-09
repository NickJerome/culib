#include "fs_watcher_metadata.h"

#include <culib/strings.h>

#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace wavvix {
namespace fs {
namespace detail {

result<snapshot_metadata> read_snapshot_metadata(string_view path) {
    result<std::wstring> wide = utf8_to_wide(path);
    if (wide.is_err()) return std::move(wide).error();

    const HANDLE handle = ::CreateFileW(wide.value().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        const error_kind kind = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
            ? error_kind::not_found
            : code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION
            ? error_kind::permission_denied : error_kind::io;
        return make_error(kind, "查询 " + path.to_string() + " 失败：Windows 错误 " +
                                std::to_string(static_cast<unsigned long long>(code)));
    }

    BY_HANDLE_FILE_INFORMATION info;
    const BOOL ok = ::GetFileInformationByHandle(handle, &info);
    const DWORD code = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);
    if (!ok) {
        return make_error(error_kind::io, "查询 " + path.to_string() + " 失败：Windows 错误 " +
                                          std::to_string(static_cast<unsigned long long>(code)));
    }

    snapshot_metadata value;
    value.device = info.dwVolumeSerialNumber;
    value.identity = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
                     info.nFileIndexLow;
    value.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
                 info.nFileSizeLow;
    value.modified = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                     info.ftLastWriteTime.dwLowDateTime;
    value.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    value.link = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    value.type = value.link ? 3u : value.directory ? 2u : 1u;
    return value;
}

}  // namespace detail
}  // namespace fs
}  // namespace wavvix
