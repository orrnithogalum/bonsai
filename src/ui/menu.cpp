#include "../../include/ui/menu.hpp"

#include "../../include/config/config.hpp"
#include "../../include/utils/format.hpp"
#include <ftxui/dom/elements.hpp>

#include <ftxui/screen/color.hpp>
#include <iomanip>
#include <sstream>

void BonsaiMenu::worker(ScreenInteractive* screen, std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path) {
    while (true) {
        std::vector<AppData::BonsaiMenuEntry> new_entries;
        std::vector<std::string> new_labels;

        fs::path current_path;
        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            current_path = *data->path;
        }

        // Always allow going back until default_path is reached
        if (!fs::equivalent(current_path, default_path)) {
            new_labels.push_back("..");
            new_entries.push_back(AppData::BonsaiMenuEntry{0, 0, "..", "", true});
        }

        try {
            std::vector<AppData::BonsaiMenuEntry> unsorted_entries;

            // Parse current path and get relevant data (size, is_dir, path, etc.)
            for (const auto& entry : fs::directory_iterator(current_path)) {
                AppData::BonsaiMenuEntry item;
                item.path = entry.path().string();
                item.label = entry.path().filename().string();
                item.is_dir = entry.is_directory();

                if (!item.is_dir) {
                    std::error_code ec;
                    item.size = entry.file_size(ec);

                    if (ec) {
                        item.size = 0;
                    }
                } else {
                    item.size = scanner->get(entry.path());
                    item.snapped_size = scanner->getSnapped(entry.path());
                }

                unsorted_entries.push_back(std::move(item));
            }

            /* Sort the aquired data:
            - Directories go first
            - Then compare size descending
            - Then compare name ascending
            */
            std::sort(unsorted_entries.begin(), unsorted_entries.end(), [](const AppData::BonsaiMenuEntry& a, const AppData::BonsaiMenuEntry& b) {
                if (a.is_dir != b.is_dir)
                    return a.is_dir;

                if (a.size != b.size)
                    return a.size > b.size;

                return a.label < b.label;
            });

            // Push the sorted data in the two array, in sync.
            for (auto& item : unsorted_entries) {
                new_labels.push_back(item.label);
                new_entries.push_back(std::move(item));
            }

        } catch (const fs::filesystem_error&) {}

        {
            // Update the menu data
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            data->menu_entries->swap(new_entries);
            data->menu_labels->swap(new_labels);
        }

        // Update render and sleep if scan is finished or new path
        screen->PostEvent(Event::Custom);

        // Sleep until woken up or keep going until scanner has completed
        if(!scanner->isDone()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(data->cv_mutex);

            data->cv.wait(lock, [&] {
                return data->menu_path_changed || data->stop;
            });

            if (data->stop)
                break;

            if (data->menu_path_changed)
                data->menu_path_changed = false;
        }
    }
}

Component BonsaiMenu::menu(ScreenInteractive* screen, std::shared_ptr<AppData::BonsaiData> data, int* selected, const fs::path& default_path, MenuOption options) {

    // Selected or unselected colors
    options.entries_option.animated_colors.foreground = AnimatedColorOption{
        .inactive = Color::White,
        .active = Color::Default
    };

    // Plot the entry data on the menu in a nice way
    options.entries_option.transform = [data](const EntryState& entry_state) {
        const auto& item = (*data->menu_entries)[entry_state.index];
        const bool is_selected = entry_state.active;
        const Config& config = Config::get();

        std::string icon_str;
        if (item.label == "..") {
            icon_str = config.SIDEBAR_BACK_ICON;
        } else {
            icon_str = is_selected ? (item.is_dir ? config.SIDEBAR_SELECTED_FOLDER_ICON : config.SIDEBAR_SELECTED_FILE_ICON)
                                   : (item.is_dir ? config.SIDEBAR_FOLDER_ICON          : config.SIDEBAR_FILE_ICON);
        }

        double growth = 0.0;
        std::string growth_str;

        if (item.is_dir && item.snapped_size != 0) {
            growth =
                (static_cast<double>(item.size) - static_cast<double>(item.snapped_size))
                / static_cast<double>(item.snapped_size)
                * 100.0;

            if (std::abs(growth) > config.SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE) {
                std::ostringstream ss;

                ss << (growth > 0 ? " " : " ")
                   << std::fixed
                   << std::setprecision(1)
                   << std::abs(growth)
                   << '%';

                growth_str = ss.str();
            }
        }

        auto row = hbox({
            text(" " + icon_str + " " + FormatUtils::trimSuffix(item.label, Config::get().SIDEBAR_WIDTH * 0.4, "...")) | size(WIDTH, EQUAL, Config::get().SIDEBAR_WIDTH * 0.6),
            filler(),

            text(item.label == ".." ? "" : growth_str) | color(growth > 0 ? Config::get().growthColor() : Config::get().shrinkColor()) | size(WIDTH, EQUAL, Config::get().SIDEBAR_WIDTH * 0.2),
            filler(),

            hbox(
                filler(),
                text(item.label == ".." ? "" : FormatUtils::toReadableShort(item.size)) | color(Color::GrayDark)
            ) | size(WIDTH, EQUAL, Config::get().SIDEBAR_WIDTH * 0.2)
        });

        return is_selected ? row | bgcolor(Color::White) | color(Color::Black)
                           : row | bgcolor(Color::Default) | color(Color::White);
    };

    // On selected: update data with a new path, and wake worker thread
    options.on_enter = [screen, data, selected]() {
        std::string new_path;
        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            if (*selected < 0 || *selected >= (int)data->menu_entries->size() || !(*data->menu_entries)[*selected].is_dir) {
                return;
            };

            new_path = (*data->menu_entries)[*selected].path;

            // For ".." go up
            if ((*data->menu_entries)[*selected].label == "..") {
                std::filesystem::path p(*data->path);
                new_path = p.parent_path().string();
            }

            *data->selected = *selected;
            *data->path = new_path;
        }

        {
            // Wake up both pie and menu on change
            std::lock_guard<std::mutex> lock(data->cv_mutex);
            data->menu_path_changed = true;
            data->pie_path_changed = true;
        }

        data->cv.notify_all();
    };

    options.on_change = [data, selected]() {
        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            if (*selected < 0 || *selected >= (int)data->menu_entries->size()) {
                return;
            }

            *data->selected = *selected;
        }

        {
            /* Only wake up pie
            - Made so that it only partially re-renders / doesn't have to compute all children again
            */
            std::lock_guard<std::mutex> lock(data->cv_mutex);
            data->pie_sel_changed = true;
        }

        data->cv.notify_all();
    };

    // Menu is updated as labels update
    return Menu(data->menu_labels.get(), selected, options);
}
