#include "src/engine.h"
#include <cmath>
#include <filesystem>
#include <stdexcept>

void check_cascade_markers(const std::filesystem::path& directory) {
    Engine engine(640, 480);
    engine.set_wait_ms(0);
    std::string error;
    if (!engine.load((directory / "cascade-markers.html").u8string(), &error) || !engine.render(false))
        throw std::runtime_error("Cascade fixture load: " + error);
    auto* doc = engine.document();
    for (const auto& item : std::vector<std::pair<const char*, float>>{
             {"class-rule",44}, {"tag-rule",44}, {"inline-rule",40},
             {"important-rule",36}, {"inline-important",32}, {"hidden",0}}) {
        auto* n = doc->get_element_by_id(item.first);
        if (!n || std::abs(n->viewport_rect().h - item.second) > .01f)
            throw std::runtime_error(std::string("UA/author/inline cascade mismatch: ") + item.first);
    }
    auto marker = [&](const char* id) {
        auto* n = doc->get_element_by_id(id);
        auto r = n->viewport_rect();
        int ink = 0;
        const auto& pixels = engine.canvas()->pixels();
        for (int y = int(r.y); y < int(r.y + r.h); ++y) {
            for (int x = int(r.x) - 18; x < int(r.x) - 3; ++x) {
                const size_t offset = (y * 640 + x) * 4;
                if (pixels[offset] < 100 && pixels[offset+1] < 100 && pixels[offset+2] < 100) ++ink;
            }
        }
        return ink > 0;
    };
    for (const char* id : {"suppressed", "inherited", "unset", "type-none", "shorthand", "invalid"})
        if (marker(id)) throw std::runtime_error(std::string("Suppressed marker was painted: ") + id);
    for (const char* id : {"control", "restored", "initial", "mixed"})
        if (!marker(id)) throw std::runtime_error(std::string("Visible marker disappeared: ") + id);
    auto toggle = doc->get_element_by_id("toggle")->viewport_rect();
    if (!engine.click(toggle.x+5, toggle.y+5) || !engine.render(false) ||
        !marker("suppressed") || !marker("inherited") || !marker("unset") ||
        doc->body->attr("data-marker") != "disc")
        throw std::runtime_error("Dynamic list marker restoration failed");
    if (!engine.click(toggle.x+5, toggle.y+5) || !engine.render(false) ||
        marker("suppressed") || marker("inherited") || marker("unset") ||
        doc->body->attr("data-marker") != "none")
        throw std::runtime_error("Dynamic list marker suppression failed");
}

// Preserve the actual model-generated semantic ul/li page that exposed the bugs.
void check_live_task(const std::filesystem::path& directory) {
    for (int width : {640, 390}) {
        Engine engine(width, width == 640 ? 480 : 844);
        engine.set_wait_ms(0);
        std::string error;
        auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        check(engine.load((directory / "live-task.html").u8string(), &error) && engine.render(false),
              "Live task fixture load");
        auto* doc = engine.document();
        auto click = [&](Node* n) {
            check(n != nullptr, "Missing live task control");
            auto r = n->viewport_rect();
            check(r.x >= 0 && r.x + r.w <= width + .1f, "Live task control outside viewport");
            return engine.click(r.x + r.w / 2, r.y + r.h / 2);
        };
        auto* input = doc->get_element_by_id("new-task");
        auto* count = doc->get_element_by_id("count");
        auto* list = doc->get_element_by_id("tasks");
        check(std::abs(input->viewport_rect().h - 44) < .01f &&
              count->text_content() == "2 tasks left", "Live task author input size/initial count");
        check(click(doc->query_selector(".add-btn")) &&
              !doc->get_element_by_id("hint")->text_content().empty(), "Live task empty feedback");
        check(click(input) && engine.type("Review observer callbacks") && engine.key("Enter") &&
              count->text_content() == "3 tasks left" &&
              list->text_content().find("Review observer callbacks") != std::string::npos,
              "Live task add/submit state");
        check(click(doc->query_selector(".task-box")) && count->text_content() == "2 tasks left",
              "Live task checkbox/change state");
        check(click(doc->query_selector("[data-filter=done]")) && engine.render(false),
              "Live task Done filter");
        int visible = 0;
        for (auto* n : list->children) {
            if (!n->is_element() || !n->layout.has_box) continue;
            ++visible;
            check(n->text_content() == "Confirm backup rotation schedule", "Wrong Done row");
        }
        check(visible == 1, "Live task filter did not hide incomplete tasks");
    }
}
