#include <cstring>
#include "html.h"
#include "util.h"
#include <algorithm>
#include <cctype>

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool is_void_tag(const std::string& t) {
    static const char* tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                 "link", "meta", "param", "source", "track", "wbr"};
    for (auto* x : tags) if (t == x) return true;
    return false;
}

bool is_head_tag(const std::string& t) {
    return t == "base" || t == "link" || t == "meta" || t == "title" || t == "style" ||
           t == "script" || t == "noscript" || t == "template";
}

bool is_raw_text_tag(const std::string& t) {
    return t == "script" || t == "style" || t == "textarea" || t == "title";
}

bool is_heading(const std::string& t) {
    return t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6";
}

size_t find_ci(const std::string& s, const std::string& needle, size_t from) {
    if (needle.empty()) return from;
    std::string lower = util::to_lower(s);
    std::string n = util::to_lower(needle);
    return lower.find(n, from);
}

std::string normalize_newlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') i++;
            out.push_back('\n');
        } else out.push_back(s[i]);
    }
    return out;
}

struct Tag {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    bool self_closing = false;
    bool valid = false;
};

class Parser {
public:
    Parser(Document& d, const std::string& s, bool frag) : doc(d), src(s), fragment(frag) {
        if (fragment) root = doc.create_document();
        else root = doc.root;
        stack.push_back(root);
    }

    Node* run() {
        while (pos < src.size()) {
            if (src[pos] == '<') {
                if (starts_with("<!--")) parse_comment();
                else if (starts_with("<!")) skip_declaration();
                else if (starts_with("<?")) skip_declaration();
                else if (pos + 1 < src.size() && src[pos + 1] == '/') parse_end_tag();
                else parse_start_tag();
            } else parse_text();
        }
        if (fragment) {
            std::vector<Node*> out;
            for (auto* c : root->children) {
                c->parent = nullptr;
                out.push_back(c);
            }
            return nullptr;
        }
        ensure_structure();
        doc.recompute_head_body();
        doc.set_title_from_dom();
        return root;
    }

    std::vector<Node*> fragment_nodes() {
        std::vector<Node*> out;
        if (!root) return out;
        out = root->children;
        for (auto* c : out) c->parent = nullptr;
        root->children.clear();
        return out;
    }

private:
    Document& doc;
    std::string src;
    bool fragment = false;
    size_t pos = 0;
    Node* root = nullptr;
    Node* html = nullptr;
    Node* head = nullptr;
    Node* body = nullptr;
    bool body_started = false;
    std::vector<Node*> stack;

    bool starts_with(const char* s) const {
        size_t n = strlen(s);
        return src.compare(pos, n, s) == 0;
    }

    Node* current() const { return stack.empty() ? root : stack.back(); }

    bool in_stack(Node* n) const {
        return std::find(stack.begin(), stack.end(), n) != stack.end();
    }

    void pop_to(Node* n) {
        while (!stack.empty() && stack.back() != n) stack.pop_back();
    }

    void ensure_html() {
        if (fragment) return;
        if (!html) {
            html = doc.create_element("html");
            root->append_child(html);
            doc.html = html;
        }
        if (!in_stack(html)) {
            while (stack.size() > 1) stack.pop_back();
            stack.push_back(html);
        }
    }

    void ensure_head() {
        if (fragment) return;
        ensure_html();
        if (!head) {
            head = doc.create_element("head");
            if (body) html->insert_before(head, body);
            else html->append_child(head);
            doc.head = head;
        }
        if (!in_stack(head)) {
            if (!in_stack(html)) stack.push_back(html);
            stack.push_back(head);
        }
    }

    void ensure_body() {
        if (fragment) return;
        ensure_html();
        if (!body_started) {
            while (stack.size() > 1 && stack.back() != html) stack.pop_back();
            if (!body) {
                body = doc.create_element("body");
                html->append_child(body);
                doc.body = body;
            }
            stack.push_back(body);
            body_started = true;
        } else if (body && !in_stack(body)) {
            stack.push_back(html);
            stack.push_back(body);
        }
    }

    void ensure_structure() {
        if (fragment) return;
        ensure_html();
        if (!head) {
            head = doc.create_element("head");
            if (body) html->insert_before(head, body);
            else html->append_child(head);
            doc.head = head;
        }
        if (!body) {
            body = doc.create_element("body");
            html->append_child(body);
            doc.body = body;
        }
    }

    Tag parse_tag() {
        Tag t;
        size_t i = pos + 1;
        while (i < src.size() && is_ws(src[i])) i++;
        size_t name_start = i;
        while (i < src.size() && !is_ws(src[i]) && src[i] != '>' && src[i] != '/') i++;
        if (i == name_start) return t;
        t.name = util::to_lower(src.substr(name_start, i - name_start));
        t.valid = true;
        while (i < src.size()) {
            while (i < src.size() && is_ws(src[i])) i++;
            if (i >= src.size()) break;
            if (src[i] == '>') { i++; break; }
            if (src[i] == '/') { t.self_closing = true; i++; continue; }
            size_t an = i;
            while (i < src.size() && !is_ws(src[i]) && src[i] != '=' && src[i] != '>' && src[i] != '/') i++;
            std::string name = util::to_lower(src.substr(an, i - an));
            while (i < src.size() && is_ws(src[i])) i++;
            std::string value;
            if (i < src.size() && src[i] == '=') {
                i++;
                while (i < src.size() && is_ws(src[i])) i++;
                if (i < src.size() && (src[i] == '"' || src[i] == '\'')) {
                    char q = src[i++];
                    size_t vs = i;
                    while (i < src.size() && src[i] != q) i++;
                    value = src.substr(vs, i - vs);
                    if (i < src.size()) i++;
                } else {
                    size_t vs = i;
                    while (i < src.size() && !is_ws(src[i]) && src[i] != '>') i++;
                    value = src.substr(vs, i - vs);
                }
            }
            if (!name.empty()) {
                bool dup = false;
                for (auto& a : t.attrs) if (a.first == name) { a.second = value; dup = true; }
                if (!dup) t.attrs.emplace_back(name, util::html_unescape(value));
            }
        }
        pos = i;
        return t;
    }

    void apply_attrs(Node* el, const Tag& t) {
        for (auto& a : t.attrs) el->set_attr(a.first, a.second);
    }

    void collect_special(Node* el) {
        if (el->is_element("style")) doc.style_nodes.push_back(el);
        else if (el->is_element("script")) doc.script_nodes.push_back(el);
        else if (el->is_element("link")) doc.link_nodes.push_back(el);
        else if (el->is_element("base") && el->has_attr("href")) {
            doc.base_url = util::resolve_url(doc.url.empty() ? doc.base_url : doc.url, el->attr("href"));
        }
    }

    void parse_start_tag() {
        Tag t = parse_tag();
        if (!t.valid) { pos++; return; }
        if (fragment) {
            // No implicit html/head/body in fragments.
            Node* parent = current();
            Node* el = doc.create_element(t.name);
            apply_attrs(el, t);
            parent->append_child(el);
            collect_special(el);
            if (is_void_tag(t.name) || t.self_closing) return;
            if (is_raw_text_tag(t.name)) {
                parse_raw_text(el, t.name);
                return;
            }
            stack.push_back(el);
            return;
        }

        if (t.name == "html") {
            ensure_html();
            apply_attrs(html, t);
            collect_special(html);
            return;
        }
        if (t.name == "head") {
            ensure_html();
            if (!head) {
                head = doc.create_element("head");
                if (body) html->insert_before(head, body);
                else html->append_child(head);
                doc.head = head;
            }
            apply_attrs(head, t);
            if (!in_stack(head)) stack.push_back(head);
            return;
        }
        if (t.name == "body") {
            ensure_html();
            while (stack.size() > 1 && stack.back() != html) stack.pop_back();
            if (!body) {
                body = doc.create_element("body");
                html->append_child(body);
                doc.body = body;
            }
            apply_attrs(body, t);
            if (!in_stack(body)) stack.push_back(body);
            body_started = true;
            return;
        }

        auto close_if_current = [&](const char* name) {
            if (current()->is_element(name)) stack.pop_back();
        };
        if (t.name == "p") close_if_current("p");
        if (t.name == "li") {
            while (stack.size() > 2 && current()->is_element("li")) stack.pop_back();
        }
        if (t.name == "option") close_if_current("option");
        if (t.name == "tr") { while (stack.size() > 2 && current()->is_element("tr")) stack.pop_back(); }
        if (t.name == "td" || t.name == "th") {
            close_if_current("td");
            close_if_current("th");
        }
        if (is_heading(t.name)) {
            if (is_heading(current()->tag)) stack.pop_back();
        }

        Node* parent = nullptr;
        if (!body_started && is_head_tag(t.name) && stack.size() > 1) {
            ensure_head();
            parent = head;
        } else {
            ensure_body();
            parent = current();
        }

        Node* el = doc.create_element(t.name);
        apply_attrs(el, t);
        parent->append_child(el);
        collect_special(el);

        if (is_void_tag(t.name) || t.self_closing) return;
        if (is_raw_text_tag(t.name)) {
            parse_raw_text(el, t.name);
            return;
        }
        stack.push_back(el);
    }

    void parse_raw_text(Node* el, const std::string& tag) {
        size_t end = pos;
        std::string needle = "</" + tag;
        while (true) {
            end = find_ci(src, needle, end);
            if (end == std::string::npos) { end = src.size(); break; }
            size_t after = end + needle.size();
            if (after >= src.size() || is_ws(src[after]) || src[after] == '>' || src[after] == '/') break;
            end = after;
        }
        std::string text = src.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? src.size() : end;
        if (tag != "script" && tag != "style") text = util::html_unescape(text);
        text = normalize_newlines(text);
        if (!text.empty()) {
            Node* tn = doc.create_text(text);
            el->append_child(tn);
        }
        // consume the closing tag
        if (pos < src.size()) {
            size_t gt = src.find('>', pos);
            pos = gt == std::string::npos ? src.size() : gt + 1;
        }
    }

    void parse_text() {
        size_t start = pos;
        while (pos < src.size() && src[pos] != '<') pos++;
        std::string text = src.substr(start, pos - start);
        if (text.empty()) return;
        std::string decoded = util::html_unescape(normalize_newlines(text));
        bool ws_only = true;
        for (char c : decoded) if (!is_ws(c)) { ws_only = false; break; }
        if (!fragment) {
            if (stack.size() <= 1 || current() == root) {
                if (ws_only) return;
                ensure_body();
            } else if (current() == head || current() == html) {
                if (ws_only) return;
                ensure_body();
            }
        }
        Node* tn = doc.create_text(decoded);
        current()->append_child(tn);
    }

    void parse_end_tag() {
        size_t i = pos + 2;
        while (i < src.size() && is_ws(src[i])) i++;
        size_t start = i;
        while (i < src.size() && !is_ws(src[i]) && src[i] != '>') i++;
        std::string name = util::to_lower(src.substr(start, i - start));
        size_t gt = src.find('>', i);
        pos = gt == std::string::npos ? src.size() : gt + 1;
        if (name.empty()) return;
        if (is_void_tag(name)) return;
        if (fragment) {
            for (int k = (int)stack.size() - 1; k >= 1; k--) {
                if (stack[k]->is_element(name)) {
                    stack.resize(k);
                    return;
                }
            }
            return;
        }
        if (name == "head") {
            if (in_stack(head)) pop_to(head);
            if (!stack.empty() && stack.back() == head) stack.pop_back();
            return;
        }
        if (name == "body") {
            if (!in_stack(body)) return;
            pop_to(body);
            if (!stack.empty() && stack.back() == body) stack.pop_back();
            return;
        }
        if (name == "html") {
            while (stack.size() > 1) stack.pop_back();
            return;
        }
        for (int k = (int)stack.size() - 1; k >= 1; k--) {
            if (stack[k]->is_element(name)) {
                stack.resize(k);
                return;
            }
        }
    }

    void parse_comment() {
        size_t end = src.find("-->", pos + 4);
        if (end == std::string::npos) { pos = src.size(); return; }
        std::string body = src.substr(pos + 4, end - pos - 4);
        pos = end + 3;
        if (!fragment) {
            if (stack.size() <= 1) return;
        }
        if (stack.empty()) return;
        Node* c = doc.create_comment(normalize_newlines(body));
        current()->append_child(c);
    }

    void skip_declaration() {
        size_t gt = src.find('>', pos + 2);
        pos = gt == std::string::npos ? src.size() : gt + 1;
    }
};

} // namespace

Node* parse_html_document(Document& doc, const std::string& source) {
    Parser p(doc, source, false);
    return p.run();
}

std::vector<Node*> parse_html_fragment(Document& doc, const std::string& source) {
    Parser p(doc, source, true);
    p.run();
    return p.fragment_nodes();
}
