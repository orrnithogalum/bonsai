#include "../../include/ui/piechart.hpp"

#include "../../include/config/config.hpp"
#include "../../include/utils/format.hpp"

#include <filesystem>
#include <algorithm>
#include <thread>
#include <string>
#include <cmath>
#include <map>
#include <set>

void BonsaiPie::drawAngledBlockEllipseRingOffset(Canvas& c, int cx, int cy, int r1, int r2, int r_inner, double start_deg, double sweep_deg, const std::string& label, const Color& color, const Color& text_color, std::vector<int>& drawn_by, int slice_id) {
    if (sweep_deg <= 0.0)
        return;

    // This could also be made user-customizable
    int max_label_length = 5;
    std::string display_label = label;
    if (display_label.length() > max_label_length) {
        display_label = display_label.substr(0, max_label_length - 2) + "..";
    }

    const double start = start_deg * M_PI / 180.0;
    const double end   = (start_deg + sweep_deg) * M_PI / 180.0;

    /* Angle step — controls smoothness vs speed
    - Smaller = smoother but slower
    - 0.3 deg is usually visually perfect in terminal
    - Make it user adjustable? idk.
    */
    const double step = 0.3 * M_PI / 180.0;

    double cos_a = std::cos(start);
    double sin_a = std::sin(start);

    // Precompute rotation delta (incremental rotation)
    const double cos_d = std::cos(step);
    const double sin_d = std::sin(step);

    const int outer_r = std::max(r1, r2);

    // Precompute mid-angle and mid-radius for text
    const double mid_angle = start + (end - start) / 2.0;
    const double mid_radius = r_inner + (outer_r - r_inner) / 2.0;
    const int text_x = (static_cast<int>(cx + std::cos(mid_angle) * mid_radius) - static_cast<int>(display_label.size()) / 2);
    const int text_y = static_cast<int>(cy + std::sin(mid_angle) * mid_radius);

    const int width = c.width();
    const int height = c.height();

    // Built once per slice instead of once per block
    const Canvas::Stylizer block_style = [color](Cell &c) {
        c.foreground_color = color;
    };

    for (double a = start; a < end; a += step) {
        const double dx_outer = cos_a * r1;
        const double dy_outer = sin_a * r2;

        const double dx_inner = cos_a * r_inner;
        const double dy_inner = sin_a * r_inner;

        const double inv_len = 1.0 / outer_r;

        for (int r = r_inner; r <= outer_r; ++r) {
            double t = r * inv_len;

            int px = static_cast<int>(cx + dx_outer * t);
            int py = static_cast<int>(cy + dy_outer * t);

            // Outside of the canvas, nothing would be drawn
            if (px < 0 || px >= width || py < 0 || py >= height)
                continue;

            /* Performance:
            - The angle steps are much finer than the blocks, so most steps land on a block this slice already drew
            - Drawing a block twice with the same color changes nothing, and DrawBlock is the expensive part
            - So each block is only drawn the first time this slice reaches it
            */
            int& owner = drawn_by[static_cast<size_t>(py / 2) * width + px];
            if (owner == slice_id)
                continue;

            owner = slice_id;
            c.DrawBlock(px, py, true, block_style);
        }

        double new_cos = cos_a * cos_d - sin_a * sin_d;
        double new_sin = sin_a * cos_d + cos_a * sin_d;

        cos_a = new_cos;
        sin_a = new_sin;
    }

    c.DrawText(text_x, text_y, display_label, [color, text_color](Cell &c) {
        c.foreground_color = text_color;
        c.background_color = color;
    });
}

// An entry gets its own slice when it weighs at least `threshold` percent of the displayed directory
static bool reachesThreshold(uint64_t size, uint64_t root_size, double threshold) {
    return root_size > 0 && size <= root_size && size * 100.0 / root_size >= threshold;
}

void BonsaiPie::collectEntries(const fs::path& dir, std::vector<EntryInfo>& entries, int current_depth, int max_depth, uint64_t root_size, double threshold, Scanner* scanner) {
    if (current_depth > max_depth) return;

    std::error_code dir_ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, dir_ec);

    for (; !dir_ec && it != fs::directory_iterator(); it.increment(dir_ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code ec;

        // The entry type comes with the directory listing: no extra system call per entry
        if (entry.is_symlink(ec))
            continue;

        const bool is_dir = entry.is_directory(ec);
        uint64_t size = 0;

        if (is_dir) {
            size = scanner->get(entry.path());

        } else if (entry.is_regular_file(ec)) {
            size = entry.file_size(ec);

            // The file disappeared in the meantime
            if (ec)
                continue;

        } else {
            continue;
        }

        /* Performance:
        - Below the top level, an entry under the size threshold is never drawn
        - Neither is anything inside of it, as a child can't be larger than its parent
        - So these entries are dropped right here and their sub-directories are never opened,
          which is most of the tree: at most 100 / threshold entries per ring can reach the threshold
        - Top-level entries are all kept: the selection and CHART_MIN_SLICES need them
        */
        const bool has_slice = reachesThreshold(size, root_size, threshold);

        if (current_depth > 0 && !has_slice)
            continue;

        entries.push_back(EntryInfo{entry.path(), size, is_dir, current_depth});

        if (is_dir && has_slice)
            collectEntries(entry.path(), entries, current_depth + 1, max_depth, root_size, threshold, scanner);
    }
}

void BonsaiPie::worker(ScreenInteractive* screen, std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path) {
    std::vector<EntryInfo> entries;

    // True when `entries` was listed during the scan, with sizes that were still growing
    bool entries_from_scan = false;

    int passes = 0;

    while (true) {

        fs::path current_path = "";
        bool sel_changed = false;
        int selected = 0;

        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            sel_changed = data->pie_sel_changed;
            current_path = *data->path;

            // If the top label is ".." it means we have to offset selected index by -1
            if (*data->selected >= 0 && *data->selected < (int)data->menu_entries->size()) {
                selected = !fs::equivalent(default_path, current_path) ? *data->selected - 1 : *data->selected;
            }

            // Don't forget to reset wake up condition
            if(data->pie_sel_changed) {
                data->pie_sel_changed = false;
            }
        }

        // Read before any size: if the scan is done at this point, every size read below is final
        bool scan_done = scanner->isDone();

        // Get size of current dir
        uint64_t root_size = scanner->get(current_path);
        Config cfg = Config::get();

        /* Only recompute children if path has changed
        - Or if they were listed during the scan: entries are filtered by size while listing,
          so a list made with unfinished sizes can't be kept
        */
        if(!sel_changed || entries_from_scan){
            entries_from_scan = !scan_done;
            entries.clear();

            // Parse current path with a max depth of 3
            collectEntries(current_path, entries, 0, cfg.CHART_MAX_GENERATIONS - 1, root_size, cfg.CHART_MAX_SIZE_THRESHOLD_PERCENTAGE, scanner);

            /* Sort:
            - Depth first: lower depth is higer priority
            - Type second: directories have a higher priority over files
            - Size third: larger sizes have a higher priority over lower ones
            - Name last: equally sized entries are in the same order as in the menu

            - Maybe we could use a priority queue here? idk.
            */
            std::sort(entries.begin(), entries.end(), [](const EntryInfo& a, const EntryInfo& b) {
                if (a.depth != b.depth)
                    return a.depth < b.depth;
                if (a.is_dir != b.is_dir)
                    return a.is_dir > b.is_dir;
                if (a.size != b.size)
                    return a.size > b.size;
                return a.path.native() < b.path.native();
            });
        }

        std::vector<AppData::BonsaiPieEntry> slices;

        int i = 0;

        /* Color & offset tracking:
        - layer_offsets_per_parent:
          Tracks the accumulated sweep (in degrees) already used for a given parent
          at its current depth layer. This determines where the next child slice
          of that parent should start (running angular total per parent).

        - slice_offsets:
          Stores the absolute starting angle of each slice (keyed by full path).
          Used so that children can inherit their parent’s base offset and stack
          correctly inside the parent’s angular span.

        - slice_colors:
          Stores the computed color and text color for each slice (keyed by full path).
          Allows child entries to derive their color from their parent’s color
          (e.g., darkening via interpolation).

        - color_indexes:
          Tracks how many slices have already been processed per depth layer per parent.
          Used to progressively adjust (e.g., darken) sibling slice colors within
          the same depth.
        */
        std::map<std::string, double> layer_offsets_per_parent;
        std::map<std::string, double> slice_offsets;

        std::map<std::pair<int, std::string>, int> color_indexes_per_parent;
        std::map<std::string, std::pair<Color, Color>> slice_colors;

        /* Minimum slices:
        - If no top-level entry reaches the size threshold (e.g. lots of equally sized entries),
          every slice gets filtered out and the pie stays empty
        - In that case the CHART_MIN_SLICES largest top-level entries are drawn anyway, at their real size
        - Nothing is forced as soon as one top-level entry reaches the threshold
        */
        std::set<const EntryInfo*> forced_slices;

        if(root_size > 0 && cfg.CHART_MIN_SLICES > 0) {
            std::vector<const EntryInfo*> candidates;
            bool has_visible_slice = false;

            for(const auto& entry : entries) {
                // Entries are sorted by depth, so top-level ones come first
                if(entry.depth != 0) {
                    break;
                }

                if(entry.size == 0 || entry.size > root_size) {
                    continue;
                }

                if(entry.size * 100.0 / root_size >= cfg.CHART_MAX_SIZE_THRESHOLD_PERCENTAGE) {
                    has_visible_slice = true;
                    break;
                }

                candidates.push_back(&entry);
            }

            if(!has_visible_slice) {
                // Stable: equally sized entries keep their current order
                std::stable_sort(candidates.begin(), candidates.end(), [](const EntryInfo* a, const EntryInfo* b) {
                    return a->size > b->size;
                });

                size_t count = std::min(candidates.size(), static_cast<size_t>(cfg.CHART_MIN_SLICES));
                forced_slices.insert(candidates.begin(), candidates.begin() + count);
            }
        }

        for(auto& entry : entries) {
            if(root_size <= 0) {
                continue;
            }

            if(entry.size > root_size) {
                continue;
            }

            /* Performance:
            - I choose only to keep 2 rings active during scanning
            - Many rings imply an even larger amount of slices
            - This causes a lot of DrawAngledBlockEllipseOffset(...) calls which are performance intensive on ui thread
            - Ftxui has cell buffer when rendering so 1rst render takes a long time with 4 rings (lots of cell updates)
            - 4 ring piechart is completed only on scan completion
            */
            if(entry.depth >= 2 && !scanner->isDone()) {
                continue;
            }

            /* Pie geometry:
            - Maybe make some of this user-customizable in the future.
            */
            int inner_hole_radius = 10;
            int inner_radius = inner_hole_radius + ((entry.depth + 1) * 25);
            int outer_radius = inner_radius + 25;

            // Kept as doubles: integer math dropped several degrees per slice, which left gaps in the rings
            double occupancy = entry.size * 100.0 / root_size;
            double sweep = entry.size * 360.0 / root_size;

            if(occupancy < cfg.CHART_MAX_SIZE_THRESHOLD_PERCENTAGE && !forced_slices.count(&entry)) {
                continue;
            }

            std::array<int, 3UL> color_values = cfg.CHART_COLORS[i % cfg.CHART_COLORS.size()];

            Color color = Color::RGB(color_values[0], color_values[1], color_values[2]);
            Color text_color = Color::Default;

            if(entry.depth != 0) {
                std::string parent = entry.path.parent_path().string();

                auto key = std::make_pair(entry.depth, parent);
                color_indexes_per_parent[key] += 1;

                color = slice_colors[parent].first;
                color = Color::Interpolate(cfg.CHART_DIM_FACTOR * color_indexes_per_parent[key], color, Color::Black);

                text_color = slice_colors[parent].second;
            }

            /* In these two code blocks, false is the logic to check wether the slice is selected
            - selected can be -1 because we manually add a back entry for non default_dir paths
            */
            if(selected != -1 && entry.depth == 0 && selected < entries.size() && entries[selected].depth == 0) {
                // Same element of the list: no need to ask the filesystem if the two paths are the same file
                bool is_selected = &entry == &entries[selected];

                color = is_selected ? Color::White : color;
                text_color = is_selected ? Color::Black : text_color;
            }

            slice_colors[entry.path.string()].first = color;
            slice_colors[entry.path.string()].second = text_color;

            AppData::BonsaiPieEntry slice;
            slice.depth = entry.depth;
            slice.color = color;
            slice.text_color = text_color;
            slice.inner_radius = inner_radius;
            slice.outer_radius = outer_radius;
            slice.label = entry.path.filename().string();
            slice.sweep = sweep;

            if(entry.depth == 0) {
                slice.offset_angle = layer_offsets_per_parent[current_path];
                layer_offsets_per_parent[current_path] += sweep;

            } else {
                std::string parent = entry.path.parent_path().string();
                slice.offset_angle = slice_offsets[parent] + layer_offsets_per_parent[parent];
                layer_offsets_per_parent[parent] += sweep;
            }

            slice_offsets[entry.path.string()] = slice.offset_angle;
            slices.push_back(slice);

            i++;
        }

        /* Possible optimisation
        - It should be possible to concatenate slices on the same layer and have a different method than DrawAngledBlockEllipseOffset(...) be called for each slice of each layer
        - This should save on rendering time as we only have to calculate our pie for each layer instead of for each slice of each layer.
        - We'll see.
        */

        {
            // Publish results
            std::lock_guard<std::mutex> lock(data->pie_mutex);
            *data->pie_entries = std::move(slices);
        }

        // Update render
        screen->PostEvent(Event::Custom);

        /* Bug fix:
        - On first render the pie will be updated with new size but not new folders
        - Probably caused by a desync between menu and pie threads? idk.
        - This forces a single extra pass/iteration (recompute everything on the first scan complete)
        */
        if(passes == 0 && scanner->isDone()) {
            passes++;
            continue;
        }

        // Sleep until woken up or keep going if scanner hasn't completed
        if(!scanner->isDone()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }


        {
            std::unique_lock<std::mutex> lock(data->pie_mutex);

            data->cv.wait(lock, [&] {
                return data->pie_path_changed || data->pie_sel_changed || data->stop;
            });

            if (data->stop)
                break;

            if (data->pie_path_changed)
                data->pie_path_changed = false;
        }
    }
}

Component BonsaiPie::pie(std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path) {
    return Renderer([data, scanner] {
        return canvas([data, scanner](Canvas& c) {
            int w = c.width();
            int h = c.height();

            // Account for margin (15) and center hole (15%)
            int max_radius = std::min(w, h) / 2 - 15;
            int inner_hole_radius = max_radius * 0.15;

            Config cfg = Config::get();
            int max_layers = cfg.CHART_MAX_GENERATIONS;

            int available_radius = max_radius - inner_hole_radius;
            int layer_thickness = available_radius / max_layers;

            std::vector<AppData::BonsaiPieEntry> entries;
            fs::path current_path;

            {
                std::lock_guard<std::mutex> lock(data->pie_mutex);
                entries = *data->pie_entries;
                current_path = *data->path;
            }

            // One marker per block of the canvas: the last slice that drew it (see drawAngledBlockEllipseRingOffset)
            std::vector<int> drawn_by(static_cast<size_t>(std::max(w, 0)) * (std::max(h, 0) / 2 + 1), -1);
            int slice_id = 0;

            for (auto& entry : entries) {

                int inner_radius = inner_hole_radius + entry.depth * layer_thickness;
                int outer_radius = inner_radius + layer_thickness;

                BonsaiPie::drawAngledBlockEllipseRingOffset(
                    c,
                    w / 2,
                    h / 2,
                    outer_radius,
                    outer_radius,
                    inner_radius,
                    entry.offset_angle,
                    entry.sweep,
                    entry.label,
                    entry.color,
                    entry.text_color,
                    drawn_by,
                    slice_id++
                );
            }

            uint64_t current_size = scanner->get(current_path);
            std::string label = FormatUtils::toReadable(current_size, " ");

            // For some reason -4 centers text better
            c.DrawText((w / 2 - label.size() / 2) - 4, h / 2, label);

            // Scanner status
            if(!scanner->isDone()) {
                c.DrawText(w  - 21, 0, "Scanning...", [](Cell& c) { c.foreground_color = Color::White; });
            }

        }) | flex;
    });
}
