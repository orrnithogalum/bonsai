/* CONFIG
Explanation:
- This class handles loading Bonsai's configuration from ~/.config/bonsai/bonsai.conf
- It provides default values if the file doesn't exist
- Configuration values include sidebar icons, chart parameters, colors, and UI sizing
- Singleton pattern ensures only one Config object exists at runtime
- The parser reads key=value lines, trims whitespace, and converts to the appropriate type
- Complex values (like CHART_COLORS) are parsed from array blocks in the config
- All file I/O, directory creation, and parsing errors are safely handled
*/

#pragma once

#include "gen/defaults.hpp"

#include <ftxui/component/component.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <optional>
#include <array>
#include <vector>
#include <unistd.h>
#include <pwd.h>
#include <string>
#include <stdexcept>

namespace fs = std::filesystem;

class Config {
public:
    std::vector<std::array<int, 3>> CHART_COLORS;

    std::string SIDEBAR_SELECTED_FOLDER_ICON;
    std::string SIDEBAR_SELECTED_FILE_ICON;
    std::string SIDEBAR_FOLDER_ICON;
    std::string SIDEBAR_FILE_ICON;
    std::string SIDEBAR_BACK_ICON;

    std::array<int, 3> SIDEBAR_GROWTH_COLOR = {255, 175, 95};
    std::array<int, 3> SIDEBAR_SHRINK_COLOR = {95, 125, 255};

    int CHART_MAX_GENERATIONS;
    int SIDEBAR_WIDTH;

    double SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE = 0.2;
    double CHART_MAX_SIZE_THRESHOLD_PERCENTAGE = 2;
    double CHART_DIM_FACTOR = 0.15;

    bool ENABLE_FOLDER_SIZE_PERCENTAGES = true;

    /* Lazy initialization using a lambda:
    - Ensures config is loaded once at first access
    - If the config file does not exist, create it with defaults
    - Parse the file, or throw if invalid
    */
    static const Config& get() {
        static Config instance = []() -> Config {
            if (!ensureConfigFile()) throw std::runtime_error("Cannot create config file");
            std::string path = getUserConfigPath();

            auto config = parseConfigFile(path);
            if (!config) throw std::runtime_error("Invalid config file");
            return *config;
        }();
        return instance;
    }

    /* Save the config file:
    - If user resizes the sidebar, then it's saved here
    - That is the only thing that can change programmatically, for now
    */
    bool writeToFile() const {
        std::string path = getUserConfigPath();
        if (path.empty())
            return false;

        std::ifstream in(path);
        if (!in.is_open())
            return false;

        std::string userConfig(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );

        in.close();

        std::string config(
            reinterpret_cast<const char*>(bonsai_default),
            bonsai_default_len
        );

        auto copyValue = [](std::string& config,
                            const std::string& source,
                            const std::string& key) -> bool {
            size_t source_key = source.find(key);

            if (source_key == std::string::npos)
                return false;

            size_t source_eq = source.find('=', source_key);

            if (source_eq == std::string::npos)
                return false;

            size_t source_value_start = source_eq + 1;

            while (source_value_start < source.size() &&
                   (source[source_value_start] == ' ' ||
                    source[source_value_start] == '\t')) {
                ++source_value_start;
            }

            size_t source_value_end = source_value_start;

            while (source_value_end < source.size() &&
                   source[source_value_end] != '\n' &&
                   source[source_value_end] != '\r' &&
                   source[source_value_end] != '#') {
                ++source_value_end;
            }

            while (source_value_end > source_value_start &&
                   (source[source_value_end - 1] == ' ' ||
                    source[source_value_end - 1] == '\t')) {
                --source_value_end;
            }

            std::string value = source.substr(
                source_value_start,
                source_value_end - source_value_start
            );

            size_t config_key = config.find(key);

            if (config_key == std::string::npos)
                return false;

            size_t config_eq = config.find('=', config_key);

            if (config_eq == std::string::npos)
                return false;

            size_t config_value_start = config_eq + 1;

            while (config_value_start < config.size() &&
                   (config[config_value_start] == ' ' ||
                    config[config_value_start] == '\t')) {
                ++config_value_start;
            }

            size_t config_value_end = config_value_start;

            while (config_value_end < config.size() &&
                   config[config_value_end] != '\n' &&
                   config[config_value_end] != '\r' &&
                   config[config_value_end] != '#') {
                ++config_value_end;
            }

            while (config_value_end > config_value_start &&
                   (config[config_value_end - 1] == ' ' ||
                    config[config_value_end - 1] == '\t')) {
                --config_value_end;
            }

            config.replace(
                config_value_start,
                config_value_end - config_value_start,
                value
            );

            return true;
        };

        copyValue(
            config,
            userConfig,
            "ENABLE_FOLDER_SIZE_PERCENTAGES"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_SELECTED_FOLDER_ICON"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_SELECTED_FILE_ICON"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_FOLDER_ICON"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_FILE_ICON"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_BACK_ICON"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_GROWTH_COLOR"
        );

        copyValue(
            config,
            userConfig,
            "SIDEBAR_SHRINK_COLOR"
        );

        copyValue(
            config,
            userConfig,
            "CHART_MAX_SIZE_THRESHOLD_PERCENTAGE"
        );

        copyValue(
            config,
            userConfig,
            "CHART_MAX_GENERATIONS"
        );

        copyValue(
            config,
            userConfig,
            "CHART_DIM_FACTOR"
        );

        copyValue(
            config,
            "SIDEBAR_WIDTH = " + std::to_string(SIDEBAR_WIDTH),
            "SIDEBAR_WIDTH"
        );

        std::ofstream out(path, std::ios::binary | std::ios::trunc);

        if (!out.is_open())
            return false;

        out.write(
            config.data(),
            static_cast<std::streamsize>(config.size())
        );

        return out.good();
    }

    static ftxui::Color growthColor() {
        const auto& config = Config::get();

        return ftxui::Color::RGB(
            config.SIDEBAR_GROWTH_COLOR[0],
            config.SIDEBAR_GROWTH_COLOR[1],
            config.SIDEBAR_GROWTH_COLOR[2]
        );
    }

    static ftxui::Color shrinkColor() {
        const auto& config = Config::get();

        return ftxui::Color::RGB(
            config.SIDEBAR_SHRINK_COLOR[0],
            config.SIDEBAR_SHRINK_COLOR[1],
            config.SIDEBAR_SHRINK_COLOR[2]
        );
    }

private:
    // Returns path to ~/.config/bonsai/bonsai.conf or empty string if HOME not set
    inline static std::string getUserConfigPath() {
        if (const auto* pw = getpwuid(getuid()); pw && pw->pw_dir) {
            return (fs::path(pw->pw_dir) / ".config/bonsai/bonsai.conf").string();
        }

        return ".config/bonsai/bonsai.conf";
    }

    // Writes the default configuration binary to the given path
    inline static bool writeDefaultConfig(const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) { return false; }

        out.write(reinterpret_cast<const char*>(bonsai_default), bonsai_default_len);
        return true;
    }

    /* Ensures the config file exists:
    - Creates parent directories if needed
    - Writes default config if file doesn't exist
    - Returns true if file is ready
    */
    inline static bool ensureConfigFile() {
        std::string config_path = getUserConfigPath();
        fs::path config_dir = fs::path(config_path).parent_path();

        if (!fs::exists(config_dir)) fs::create_directories(config_dir);
        if (!fs::exists(config_path)) return writeDefaultConfig(config_path);

        return true;
    }

    /* Reads a config file and populates a Config object:
    - Ignores empty lines and comments (#)
    - Splits lines by '=' into key/value
    - Trims leading/trailing spaces and tabs
    - Converts values to int, double, or string as appropriate
    - Special handling for CHART_COLORS array block
    */
    inline static std::optional<Config> parseConfigFile(const std::string& config_path) {
        std::ifstream in(config_path);
        if (!in.is_open()) return std::nullopt;

        Config cfg;
        std::string line;

        // Trim leading and trailing whitespace
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };

        while (std::getline(in, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#')
                continue;

            // Skip malformed lines
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            trim(key);
            trim(value);

            try {
                if (key == "SIDEBAR_WIDTH")
                    cfg.SIDEBAR_WIDTH = std::stoi(value);

                else if (key == "SIDEBAR_SELECTED_FOLDER_ICON")
                    cfg.SIDEBAR_SELECTED_FOLDER_ICON = value.substr(1, value.size() - 2);

                else if (key == "SIDEBAR_SELECTED_FILE_ICON")
                    cfg.SIDEBAR_SELECTED_FILE_ICON = value.substr(1, value.size() - 2);

                else if (key == "SIDEBAR_FOLDER_ICON")
                    cfg.SIDEBAR_FOLDER_ICON = value.substr(1, value.size() - 2);

                else if (key == "SIDEBAR_FILE_ICON")
                    cfg.SIDEBAR_FILE_ICON = value.substr(1, value.size() - 2);

                else if (key == "SIDEBAR_BACK_ICON")
                    cfg.SIDEBAR_BACK_ICON = value.substr(1, value.size() - 2);

                else if (key == "SIDEBAR_GROWTH_COLOR") {
                    std::stringstream ss(value);
                    char c;

                    ss >> c >> cfg.SIDEBAR_GROWTH_COLOR[0]
                       >> c >> cfg.SIDEBAR_GROWTH_COLOR[1]
                       >> c >> cfg.SIDEBAR_GROWTH_COLOR[2];
                }

                else if (key == "SIDEBAR_SHRINK_COLOR") {
                    std::stringstream ss(value);
                    char c;

                    ss >> c >> cfg.SIDEBAR_SHRINK_COLOR[0]
                       >> c >> cfg.SIDEBAR_SHRINK_COLOR[1]
                       >> c >> cfg.SIDEBAR_SHRINK_COLOR[2];
                }

                else if (key == "CHART_MAX_SIZE_THRESHOLD_PERCENTAGE")
                    cfg.CHART_MAX_SIZE_THRESHOLD_PERCENTAGE = std::stod(value);

                else if (key == "CHART_MAX_GENERATIONS")
                    cfg.CHART_MAX_GENERATIONS = std::stoi(value);

                else if (key == "CHART_DIM_FACTOR")
                    cfg.CHART_DIM_FACTOR = std::stod(value);

                else if (key == "CHART_COLORS") {
                    std::string colors_block;

                    if (value.find('[') != std::string::npos) {
                        colors_block += value.substr(value.find('[') + 1);
                        std::string line2;

                        while (std::getline(in, line2)) {
                            if (line2.find(']') != std::string::npos) {
                                colors_block += line2.substr(0, line2.find(']'));
                                break;
                            }
                            colors_block += line2;
                        }
                    }

                    // Parse RGB tuples from the block
                    std::stringstream ss(colors_block);
                    std::string tuple;

                    while (std::getline(ss, tuple, '}')) {
                        size_t start = tuple.find('{');

                        if (start == std::string::npos)
                            continue;

                        tuple = tuple.substr(start + 1);
                        std::stringstream ts(tuple);
                        std::array<int, 3> color{};

                        for (int i = 0; i < 3 && ts.good(); i++) {
                            std::string num;
                            std::getline(ts, num, ',');
                            color[i] = std::stoi(num);
                        }

                        cfg.CHART_COLORS.push_back(color);
                    }
                }

                else if (key == "ENABLE_FOLDER_SIZE_PERCENTAGES") {
                    cfg.ENABLE_FOLDER_SIZE_PERCENTAGES = (value == "true");
                }

            } catch (...) {
                return std::nullopt;
            }
        }

        return cfg;
    }
};
