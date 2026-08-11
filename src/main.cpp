#include "../include/ui/modal_confirm.hpp"
#include "../include/ui/modal_error.hpp"
#include "../include/config/config.hpp"
#include "../include/core/app_data.hpp"
#include "../include/core/scanner.hpp"
#include "../include/ui/piechart.hpp"
#include "../include/ui/menu.hpp"

#include <cstdio>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <filesystem>

#include <iostream>
#include <unistd.h>
#include <thread>
#include <pwd.h>

/* TODO: bug? (can't reproduce anymore...)
-> scanner isn't finished.
-> menu scans for a folder, and updates sizes in list
-> this updates render, that updates piechart.
-> piechart worker doesn't wake up
-> for some reason percentages are updated but folder list isn't?

-> Maybe fixable with callback on scan completed (update render on complete) since this only happens when scanner is done
*/

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    using namespace ftxui;

    auto config = Config::get();
    auto screen = ScreenInteractive::Fullscreen();
    auto default_path = argc == 1 ? fs::current_path() : fs::weakly_canonical(argv[1]);

    if(!fs::exists(default_path)) {
        std::cerr << "Error: Path does not exist: " << default_path << std::endl;
        return 1;
    }

    if(!fs::is_directory(default_path)) {
        std::cerr << "Error: Path isn't a directory: " << default_path << std::endl;
        return 1;
    }

    // Launch scanner
    auto home = fs::path(getpwuid(getuid())->pw_dir);
    auto db_path = fs::weakly_canonical(home / ".cache/bonsai/db.txt");

    Scanner scanner = Scanner(default_path, db_path);
    std::thread scanner_thread([&scanner]() {
        scanner.scan();
        scanner.snapshot();
    });

    // Init shared menu & pie data
    int selected = 0;
    auto data = std::make_shared<AppData::BonsaiData>();

    data->menu_entries = std::make_shared<std::vector<AppData::BonsaiMenuEntry>>();
    data->menu_labels = std::make_shared<std::vector<std::string>>();

    data->path = std::make_shared<std::string>(default_path);
    data->selected = std::make_shared<int>(0);

    /* Init confirm & error modals
    - Allow quitting modal with button
    - Allow quitting modal with escape key
    */
    bool show_confirm_modal = false;
    bool show_error_modal = false;
    Scanner::ScannerRemoveResult result = Scanner::ScannerRemoveResult{"", false};

    auto confirm_modal = BonsaiModalConfirm::confirm(
        [data, &scanner, &result, &show_confirm_modal, &show_error_modal](){
            fs::path selected_path;

            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);
                selected_path = (*data->menu_entries)[*data->selected].path;
            }

            result = scanner.remove(selected_path);

            if(result.failed) {
                show_error_modal = true;
            }

            show_confirm_modal = false;

            {
                // Wake up both pie and menu on change
                std::lock_guard<std::mutex> lock(data->cv_mutex);
                data->menu_path_changed = true;
                data->pie_path_changed = true;
            }

            data->cv.notify_all();
        },
        [&]{
            show_confirm_modal = false;
        }
    );

    confirm_modal = CatchEvent(confirm_modal, [&show_confirm_modal](Event event) {
        if(event == Event::Escape) {
            show_confirm_modal = false;
            return true;
        }

        return false;
    });

    auto error_modal = BonsaiModalError::error(result.reason,
        [&show_error_modal](){
            show_error_modal = false;
        }
    );

    error_modal = CatchEvent(error_modal, [&show_error_modal](Event event) {
        if(event == Event::Escape) {
            show_error_modal = false;
            return true;
        }

        return false;
    });

    /* Init menu:
    - Use a container to keep focus through the entire render.
      If a menu component is embeded in a slew of elements, it becomes static
    - Allow going back in tree with escape or backspace keys
    */
    MenuOption option;

    auto menu_component = BonsaiMenu::menu(&screen, data, &selected, default_path, option);
    menu_component = CatchEvent(menu_component, [data, &scanner, &default_path, &show_confirm_modal](Event event) {
        if (event == Event::Delete) {
            show_confirm_modal = true;
            return true;
        }

        if(event == Event::Backspace || event == Event::Escape) {
            std::string new_path;
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);

                if(fs::equivalent(*data->path, default_path)) {
                    return true;
                }

                std::filesystem::path p(*data->path);
                new_path = p.parent_path().string();

                *data->path = new_path;
            }

            {
                // Wake up both pie and menu on change
                std::lock_guard<std::mutex> lock(data->cv_mutex);
                data->menu_path_changed = true;
                data->pie_path_changed = true;
            }

            data->cv.notify_all();
            return true;
        }

        return false;
    });

    auto menu_container = Container::Vertical({menu_component});
    std::thread menu_thread(BonsaiMenu::worker, &screen, data, &scanner, default_path);


    /* Init pie:
    - No need for a containter this time, as the component isn't interactive
    */
    data->pie_entries = std::make_shared<std::vector<AppData::BonsaiPieEntry>>();

    auto pie_component = BonsaiPie::pie(data, &scanner, default_path);
    std::thread pie_thread(BonsaiPie::worker, &screen, data, &scanner, default_path);

    auto left = Renderer(menu_container, [&] {
        std::lock_guard<std::mutex> lock_m(data->menu_mutex);

        return vbox({
            text(" "),
            text("Explorer") | center | bold,
            separator(),
            hbox({
                menu_component->Render() | frame | flex,
                text(" ")
            })
        });
    });

    auto window = ResizableSplitLeft(left, pie_component, &config.SIDEBAR_WIDTH);

    window |= Modal(confirm_modal, &show_confirm_modal);
    window |= Modal(error_modal, &show_error_modal);

    // Quit with q, allow sidebar resize with arrow keys
    auto app = CatchEvent(window, [&](Event event) {
        if (event == Event::Character('q') || event == Event::Character('Q')) {
            screen.Exit();
            return true;
        }

        if(event == Event::ArrowRight && !show_confirm_modal && !show_error_modal) {
            config.SIDEBAR_WIDTH += 1;
            return true;
        }

        if(event == Event::ArrowLeft && !show_confirm_modal && !show_error_modal) {
            config.SIDEBAR_WIDTH -= 1;
            return true;
        }

        return false;
    });

    // Render UI
    screen.Loop(app);


    // Stop app
    scanner.stop();
    AppData::stop(data);

    // Save config
    config.writeToFile();

    scanner_thread.join();
    menu_thread.join();
    pie_thread.join();

    return 0;
}
