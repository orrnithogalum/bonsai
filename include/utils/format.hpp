#pragma once

#include <sstream>
#include <iomanip>
#include <cstdint>
#include <string>

class FormatUtils {
public:
    // Just convert a size in bytes to a human readable format
    static std::string toReadable(uintmax_t bytes, const std::string& spacer = " ") {
        constexpr uintmax_t KB = 1024;
        constexpr uintmax_t MB = KB * 1024;
        constexpr uintmax_t GB = MB * 1024;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);

        if (bytes >= GB)
            oss << (double)bytes / GB << spacer << "GiB";
        else if (bytes >= MB)
            oss << (double)bytes / MB << spacer << "MiB";
        else if (bytes >= KB)
            oss << (double)bytes / KB << spacer << "KiB";
        else
            oss << bytes << spacer << "B";

        return oss.str();
    }

    // Same thing without the spacer argument
    static std::string toReadableShort(uintmax_t bytes) {
        return toReadable(bytes, "");
    }

    static std::string trimSuffix(const std::string& str, size_t max_size, const std::string& suffix) {
        if (str.size() <= max_size)
            return str;

        if (suffix.size() >= max_size)
            return suffix.substr(0, max_size);

        size_t cut = max_size - suffix.size();
        return str.substr(0, cut) + suffix;
    }
};
