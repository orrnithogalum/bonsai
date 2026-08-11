/* APP DATA
- Shared data between all bonsai components
*/

#pragma once

#include <ftxui/screen/color.hpp>
#include <condition_variable>
#include <vector>
#include <mutex>

using namespace ftxui;

class AppData {
public:
    /* BonsaiMenuEntry
    - Represents a single filesystem entry in the menu.
    - Contains:
      size   → folder size (0 for directories)
      label  → display name in the menu
      path   → absolute filesystem path
      is_dir → whether entry is a directory
    */
    struct BonsaiMenuEntry {
        std::uintmax_t size;
        std::uintmax_t snapped_size;
        std::string label;
        std::string path;

        bool is_dir;
    };

    struct BonsaiPieEntry {
        std::string label;
        Color text_color;
        Color color;

        int inner_radius;
        int outer_radius;
        int offset_angle;
        int sweep;
        int depth;
    };

    /* BonsaiMenuData
    - Shared state between UI and worker thread.
    - entries_mutex protects entries/labels updates.
    - cv_mutex + condition variable coordinate worker sleeping/waking.
    - path_changed signals directory navigation.
    - stop signals worker termination.
    */
    struct BonsaiData {
        std::mutex menu_mutex;
        std::shared_ptr<std::vector<BonsaiMenuEntry>> menu_entries;
        std::shared_ptr<std::vector<std::string>> menu_labels;

        std::mutex pie_mutex;
        std::shared_ptr<std::vector<BonsaiPieEntry>> pie_entries;

        std::shared_ptr<std::string> path;
        std::shared_ptr<int> selected;

        std::mutex cv_mutex;
        std::condition_variable cv;

        bool menu_path_changed = false;
        bool pie_path_changed = false;
        bool pie_sel_changed = false;
        bool stop = false;

    };

    /* stop()
    - Sets done flag to true.
    - Any thread created will a data struct is watching that flag.
    - Intended to be called during application exit.
    */
    static void stop(std::shared_ptr<AppData::BonsaiData> data);
};
