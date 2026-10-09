/* PIE
New pie structure: no class inheriting, just using base ftxui components (more maintainable)
This version of the file is a standalone version (no menu, but full contents)
Note that directory sizes aren't computed (scanner's job)

- Retrieve current directory total size from Scanner.
- Retrieve all scanned entries up to a given max depth.
- For each entry:
    - Get its size.
    - Compute its percentage relative to the current root directory.
    - Convert that percentage into an angular sweep.
- Each depth level corresponds to one ring on the canvas:
    - Depth 0 -> inner ring.
    - Higher depths -> outer rings.
- Entries are sorted by:
    - Depth (ascending).
    - Type (directories first).
    - Size (descending).
- For each computed angle:
    - Accumulate offset angle.
    - Create a slice entry with inner/outer radius.
    - Push slice to render list.
- Inner ring defines base color.
- Outer rings will later dim or derive color from parent.
- Rendering draws ring sectors using computed angle and offset.
*/

# pragma once

#include "../../include/core/app_data.hpp"
#include "../../include/core/scanner.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>

using namespace ftxui;

class BonsaiPie {
public:
    static Component pie(std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path);
    static void worker(ScreenInteractive* screen, std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path);

private:
    struct EntryInfo {
        std::filesystem::path path;
        uint64_t size;
        
        bool is_dir;
        int depth;
    };

    /* Lists what the pie can draw under `dir`, down to `max_depth`.
    - Top-level entries (depth 0) are all listed.
    - Deeper entries are only listed if they reach `threshold` percent of `root_size`,
      and only those directories are opened.
    */
    static void collectEntries(const fs::path& dir, std::vector<EntryInfo>& entries, int current_depth, int max_depth, uint64_t root_size, double threshold, Scanner* scanner);

    /* Draws a ring-shaped sector of an ellipse onto the Canvas.
    Parameters:
        - cx, cy       -> Center of the ellipse.
        - r1, r2       -> Outer radii along X and Y axes (supports non-uniform scaling).
        - r_inner      -> Inner radius (creates the hollow ring effect).
        - start_deg    -> Starting angle in degrees.
        - sweep_deg    -> Angular sweep in degrees.
        - label        -> Text displayed inside the slice (truncated if too long).
        - color        -> Fill color of the slice.
        - text_color   -> Color of the label text.
        - drawn_by     -> One int per block of the canvas (width * (height / 2 + 1)), set to -1.
                          Shared by all the slices of a frame.
        - slice_id     -> Number of this slice in the frame (0, 1, 2...).

    Behavior:
        - Converts angles from degrees to radians.
        - Iterates from start angle to end angle using a small angular step
        (controls smoothness vs performance).
        - Uses incremental rotation (cos/sin delta) to avoid repeated trig calls.
        - For each angle step:
            - Interpolates radially from r_inner to outer radius.
            - Draws blocks that fall within the elliptical boundary,
              once per block (drawn_by remembers which slice drew each block last).
        - Computes mid-angle and mid-radius to position the label
        approximately centered inside the slice.
        - Renders the label with foreground/background color contrast.

    Notes:
        - Although this supports true ellipses (r1 != r2),
        it is currently used with r1 == r2 (i.e., circles).
        - Terminal character aspect ratios can visually distort circles.
        r1 and r2 could be adjusted in the future to compensate for
        vertical text scaling differences in FTXUI.
        - Angular step (currently ~0.4°) is a visual/performance trade-off
        and could be made configurable.
    */
    static void drawAngledBlockEllipseRingOffset(Canvas& c, int x1, int y1, int r1, int r2, int r3, double starting_angle, double angle, const std::string& label, const Color& color, const Color& text_color, std::vector<int>& drawn_by, int slice_id);
};