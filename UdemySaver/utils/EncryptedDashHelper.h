#pragma once

#include <functional>
#include <string>
#include <vector>

class EncryptedDashHelper {
public:
    static bool download(
        const std::string& mpd_url,
        const std::string& output_base,
        const std::vector<std::string>& extra_headers,
        const std::string& proxy,
        const std::string& license_url,
        std::function<bool(double, double)> on_progress,
        std::string& msg);
};
