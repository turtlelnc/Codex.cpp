#include "css.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace {

bool is_ident_char(char c) {
    unsigned char u = (unsigned char)c;
    return std::isalnum(u) || c == '_' || c == '-' || u >= 0x80;
}

std::string read_ident(const std::string& s, size_t& i) {
    std::string out;
    while (i < s.size()) {
        char c = s[i];
        if (is_ident_char(c)) { out.push_back(c); i++; }
        else if (c == '\\' && i + 1 < s.size()) { out.push_back(s[i + 1]); i += 2; }
        else break;
    }
    return out;
}

void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
}

std::string strip_comments(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    for (size_t i = 0; i < css.size();) {
        char c = css[i];
        if (c == '"' || c == '\'') {
            char q = c;
            out.push_back(css[i++]);
            while (i < css.size()) {
                out.push_back(css[i]);
                if (css[i] == '\\' && i + 1 < css.size()) {
                    out.push_back(css[i + 1]);
                    i += 2;
                    continue;
                }
                if (css[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        out.push_back(c);
        i++;
    }
    return out;
}

std::vector<std::string> split_top(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    int par = 0, bracket = 0, brace = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < s.size()) { cur.push_back(s[i + 1]); i++; }
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur.push_back(c); continue; }
        if (c == '(') par++;
        else if (c == ')') par--;
        else if (c == '[') bracket++;
        else if (c == ']') bracket--;
        else if (c == '{') brace++;
        else if (c == '}') brace--;
        if (c == delim && par == 0 && bracket == 0 && brace == 0) {
            out.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

size_t find_matching(const std::string& s, size_t open, char o, char c) {
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i < s.size(); i++) {
        char ch = s[i];
        if (quote) {
            if (ch == '\\') { i++; continue; }
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') { quote = ch; continue; }
        if (ch == o) depth++;
        else if (ch == c) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

struct SelectorParseResult {
    bool ok = false;
    Selector selector;
};

SelectorParseResult parse_one_selector(const std::string& text);

std::vector<Selector> parse_selector_list_impl(const std::string& text) {
    std::vector<Selector> out;
    for (auto& part : split_top(text, ',')) {
        std::string p = util::trim(part);
        if (p.empty()) continue;
        auto r = parse_one_selector(p);
        if (r.ok) out.push_back(r.selector);
    }
    return out;
}

int max_spec_id(const std::vector<Selector>& sels) {
    int v = 0;
    for (auto& s : sels) v = std::max(v, s.spec_id);
    return v;
}
int max_spec_class(const std::vector<Selector>& sels) {
    int v = 0;
    for (auto& s : sels) v = std::max(v, s.spec_class);
    return v;
}
int max_spec_tag(const std::vector<Selector>& sels) {
    int v = 0;
    for (auto& s : sels) v = std::max(v, s.spec_tag);
    return v;
}

bool parse_compound(const std::string& s, size_t& i, CompoundSelector& comp, Selector& sel) {
    bool any = false;
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace((unsigned char)c) || c == '>' || c == '+' || c == '~' || c == ',') break;
        if (c == '*') {
            comp.tag = "*";
            i++;
            any = true;
        } else if (c == '#') {
            i++;
            std::string id = read_ident(s, i);
            if (id.empty()) return false;
            comp.id = id;
            sel.spec_id++;
            any = true;
        } else if (c == '.') {
            i++;
            std::string cls = read_ident(s, i);
            if (cls.empty()) return false;
            comp.classes.push_back(cls);
            sel.spec_class++;
            any = true;
        } else if (c == '[') {
            i++;
            skip_ws(s, i);
            AttrSelector a;
            a.name = util::to_lower(read_ident(s, i));
            if (a.name.empty()) return false;
            skip_ws(s, i);
            static const char* ops[] = {"~=", "^=", "$=", "*=", "|=", "="};
            for (auto* op : ops) {
                size_t n = strlen(op);
                if (s.compare(i, n, op) == 0) { a.op = op; i += n; break; }
            }
            if (!a.op.empty()) {
                skip_ws(s, i);
                if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                    char q = s[i++];
                    while (i < s.size() && s[i] != q) {
                        if (s[i] == '\\' && i + 1 < s.size()) { a.value.push_back(s[i + 1]); i += 2; }
                        else a.value.push_back(s[i++]);
                    }
                    if (i < s.size()) i++;
                } else {
                    size_t start = i;
                    while (i < s.size() && s[i] != ']' && !std::isspace((unsigned char)s[i])) i++;
                    a.value = s.substr(start, i - start);
                }
            }
            skip_ws(s, i);
            if (i >= s.size() || s[i] != ']') return false;
            i++;
            comp.attrs.push_back(a);
            sel.spec_class++;
            any = true;
        } else if (c == ':') {
            i++;
            bool pseudo_element = false;
            if (i < s.size() && s[i] == ':') { pseudo_element = true; i++; }
            std::string name = util::to_lower(read_ident(s, i));
            if (name.empty()) return false;
            std::string arg;
            if (i < s.size() && s[i] == '(') {
                size_t close = find_matching(s, i, '(', ')');
                if (close == std::string::npos) return false;
                arg = util::trim(s.substr(i + 1, close - i - 1));
                i = close + 1;
            }
            if (pseudo_element) {
                comp.has_pseudo_element = true;
                sel.spec_tag++;
                any = true;
                continue;
            }
            if (name == "not") {
                auto subs = parse_selector_list_impl(arg);
                if (subs.empty()) return false;
                sel.spec_id += max_spec_id(subs);
                sel.spec_class += max_spec_class(subs);
                sel.spec_tag += max_spec_tag(subs);
                for (auto& sub : subs) comp.nots.push_back(std::make_shared<Selector>(sub));
                any = true;
            } else if (name == "is" || name == "matches" || name == "where") {
                auto subs = parse_selector_list_impl(arg);
                if (subs.empty()) return false;
                if (name != "where") {
                    sel.spec_id += max_spec_id(subs);
                    sel.spec_class += max_spec_class(subs);
                    sel.spec_tag += max_spec_tag(subs);
                }
                for (auto& sub : subs) comp.anys.push_back(std::make_shared<Selector>(sub));
                any = true;
            } else if (name == "has") {
                comp.pseudos.push_back("has");
                comp.pseudo_args.emplace_back("has", arg);
                sel.spec_class++;
                any = true;
            } else {
                comp.pseudos.push_back(name);
                if (!arg.empty()) comp.pseudo_args.emplace_back(name, arg);
                sel.spec_class++;
                any = true;
            }
        } else if (is_ident_char(c)) {
            std::string tag = util::to_lower(read_ident(s, i));
            comp.tag = tag;
            sel.spec_tag++;
            any = true;
        } else {
            i++;
            return false;
        }
    }
    return any;
}

SelectorParseResult parse_one_selector(const std::string& text) {
    SelectorParseResult r;
    Selector sel;
    size_t i = 0;
    skip_ws(text, i);
    while (i < text.size()) {
        CompoundSelector comp;
        if (!parse_compound(text, i, comp, sel)) return r;
        if (comp.has_pseudo_element) return r; // unsupported pseudo-element
        sel.parts.emplace_back(SelectorPart{std::move(comp)});
        bool had_ws = false;
        while (i < text.size() && std::isspace((unsigned char)text[i])) { i++; had_ws = true; }
        if (i >= text.size()) break;
        char c = text[i];
        if (c == '>' || c == '+' || c == '~') {
            sel.combinators.push_back(c);
            i++;
            skip_ws(text, i);
            if (i >= text.size()) return r;
        } else if (had_ws) {
            sel.combinators.push_back(' ');
        } else {
            return r;
        }
    }
    if (sel.parts.empty()) return r;
    if (sel.combinators.size() + 1 != sel.parts.size()) return r;
    r.ok = true;
    r.selector = std::move(sel);
    return r;
}

bool nth_match(const std::string& expr, int index) {
    std::string s = util::to_lower(expr);
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return std::isspace((unsigned char)c); }), s.end());
    if (s.empty()) return false;
    if (s == "odd") return index % 2 == 1;
    if (s == "even") return index % 2 == 0;
    size_t npos = s.find('n');
    if (npos == std::string::npos) {
        try { return index == std::stoi(s); } catch (...) { return false; }
    }
    std::string a_str = s.substr(0, npos);
    std::string b_str = s.substr(npos + 1);
    int a = 1;
    if (!a_str.empty()) {
        if (a_str == "+") a = 1;
        else if (a_str == "-") a = -1;
        else { try { a = std::stoi(a_str); } catch (...) { return false; } }
    }
    int b = 0;
    if (!b_str.empty()) {
        try { b = std::stoi(b_str); } catch (...) { return false; }
    }
    if (a == 0) return index == b;
    int diff = index - b;
    if (diff % a != 0) return false;
    int n = diff / a;
    return n >= 0;
}

const Node* prev_element(const Node* n) {
    if (!n || !n->parent) return nullptr;
    auto& ch = n->parent->children;
    auto it = std::find(ch.begin(), ch.end(), n);
    if (it == ch.end()) return nullptr;
    while (it != ch.begin()) {
        --it;
        if ((*it)->is_element()) return *it;
    }
    return nullptr;
}

const Node* next_element(const Node* n) {
    if (!n || !n->parent) return nullptr;
    auto& ch = n->parent->children;
    auto it = std::find(ch.begin(), ch.end(), n);
    if (it == ch.end()) return nullptr;
    ++it;
    for (; it != ch.end(); ++it) if ((*it)->is_element()) return *it;
    return nullptr;
}

int element_index(const Node* n, bool same_type) {
    if (!n || !n->parent) return 0;
    int idx = 0;
    for (auto* c : n->parent->children) {
        if (!c->is_element()) continue;
        if (same_type && c->tag != n->tag) continue;
        idx++;
        if (c == n) return idx;
    }
    return 0;
}

bool match_compound(const Node* n, const CompoundSelector& comp, const Document* doc);
bool match_selector_from(const Node* n, const Selector& sel, int idx, const Document* doc);

bool match_pseudo(const Node* n, const std::string& name, const std::string& arg, const CompoundSelector& comp,
                  const Document* doc) {
    if (name == "root") {
        if (doc && doc->html) return n == doc->html;
        return n->is_element("html") && (!n->parent || n->parent->is_document());
    }
    if (name == "first-child") return prev_element(n) == nullptr;
    if (name == "last-child") return next_element(n) == nullptr;
    if (name == "only-child") return prev_element(n) == nullptr && next_element(n) == nullptr;
    if (name == "first-of-type") {
        int idx = element_index(n, true);
        return idx == 1;
    }
    if (name == "last-of-type") {
        if (!n->parent) return false;
        for (auto it = n->parent->children.rbegin(); it != n->parent->children.rend(); ++it) {
            if ((*it)->is_element() && (*it)->tag == n->tag) return *it == n;
        }
        return false;
    }
    if (name == "only-of-type") {
        return element_index(n, true) == 1 &&
               [&]() {
                   int count = 0;
                   for (auto* c : n->parent->children) if (c->is_element() && c->tag == n->tag) count++;
                   return count == 1;
               }();
    }
    if (name == "nth-child") return nth_match(arg, element_index(n, false));
    if (name == "nth-of-type") return nth_match(arg, element_index(n, true));
    if (name == "nth-last-child" || name == "nth-last-of-type") {
        if (!n->parent) return false;
        int idx = 0;
        for (auto it = n->parent->children.rbegin(); it != n->parent->children.rend(); ++it) {
            if (!(*it)->is_element()) continue;
            if (name == "nth-last-of-type" && (*it)->tag != n->tag) continue;
            idx++;
            if (*it == n) break;
        }
        return nth_match(arg, idx);
    }
    if (name == "empty") {
        for (auto* c : n->children) {
            if (c->is_element()) return false;
            if (c->is_text() && !c->data.empty()) return false;
        }
        return true;
    }
    if (name == "checked") {
        if (n->has_attr("checked") || n->has_attr("selected")) return true;
        return false;
    }
    if (name == "disabled") return n->has_attr("disabled");
    if (name == "enabled") {
        static const char* form[] = {"button", "input", "select", "textarea", "option", "optgroup", "fieldset"};
        for (auto* t : form) if (n->is_element(t)) return !n->has_attr("disabled");
        return false;
    }
    if (name == "required") return n->has_attr("required");
    if (name == "optional") return !n->has_attr("required");
    if (name == "read-only") return n->has_attr("readonly") || n->is_element("input") || n->is_element("textarea");
    if (name == "read-write") return !n->has_attr("readonly");
    if (name == "link" || name == "any-link") return n->is_element("a") && n->has_attr("href");
    if (name == "not") return false; // handled via comp.nots
    if (name == "has") {
        // Descendant form of :has() is useful enough for the common case.
        std::string sub = util::trim(arg);
        bool child_only = false;
        if (!sub.empty() && sub[0] == '>') { child_only = true; sub = util::trim(sub.substr(1)); }
        if (sub.empty()) return false;
        auto sels = parse_selector_list_impl(sub);
        if (sels.empty()) return false;
        std::function<bool(const Node*)> visit = [&](const Node* x) {
            for (auto* c : x->children) {
                if (!c->is_element()) continue;
                for (auto& s : sels) if (css_matches_selector(c, s, doc)) return true;
                if (!child_only && visit(c)) return true;
            }
            return false;
        };
        return visit(n);
    }
    // dynamic states are false at rest
    if (name == "hover" || name == "active" || name == "focus" || name == "focus-within" ||
        name == "focus-visible" || name == "visited" || name == "target" || name == "placeholder-shown")
        return false;
    return false;
}

bool match_compound(const Node* n, const CompoundSelector& comp, const Document* doc) {
    if (!n || !n->is_element()) return false;
    if (!comp.tag.empty() && comp.tag != "*" && n->tag != comp.tag) return false;
    if (!comp.id.empty() && n->attr("id") != comp.id) return false;
    for (auto& c : comp.classes) if (!n->has_class(c)) return false;
    for (auto& a : comp.attrs) {
        if (!n->has_attr(a.name)) return false;
        if (a.op.empty()) continue;
        std::string v = n->attr(a.name);
        if (a.op == "=" && v != a.value) return false;
        if (a.op == "~=") {
            bool found = false;
            for (auto& w : util::split_ws(v)) if (w == a.value) found = true;
            if (!found) return false;
        } else if (a.op == "^=") {
            if (!util::starts_with(v, a.value)) return false;
        } else if (a.op == "$=") {
            if (!util::ends_with(v, a.value)) return false;
        } else if (a.op == "*=") {
            if (v.find(a.value) == std::string::npos) return false;
        } else if (a.op == "|=") {
            if (v != a.value && !util::starts_with(v, a.value + "-")) return false;
        }
    }
    for (auto& p : comp.pseudos) {
        std::string arg;
        for (auto& pa : comp.pseudo_args) if (pa.first == p) { arg = pa.second; break; }
        if (!match_pseudo(n, p, arg, comp, doc)) return false;
    }
    for (auto& s : comp.nots) {
        if (css_matches_selector(n, *s, doc)) return false;
    }
    if (!comp.anys.empty()) {
        for (auto& s : comp.anys) {
            if (css_matches_selector(n, *s, doc)) return true;
        }
        return false;
    }
    return true;
}

bool match_selector_from(const Node* n, const Selector& sel, int idx, const Document* doc) {
    if (idx < 0) return true;
    if (!n || !n->is_element()) return false;
    if (!match_compound(n, sel.parts[idx].compound, doc)) return false;
    if (idx == 0) return true;
    char comb = idx - 1 < (int)sel.combinators.size() ? sel.combinators[idx - 1] : ' ';
    if (comb == '>') {
        const Node* p = n->parent;
        while (p && !p->is_element()) p = p->parent;
        return p && match_selector_from(p, sel, idx - 1, doc);
    }
    if (comb == ' ') {
        for (const Node* p = n->parent; p; p = p->parent) {
            if (p->is_element() && match_selector_from(p, sel, idx - 1, doc)) return true;
        }
        return false;
    }
    if (comb == '+') {
        const Node* p = prev_element(n);
        return p && match_selector_from(p, sel, idx - 1, doc);
    }
    if (comb == '~') {
        for (const Node* p = prev_element(n); p; p = prev_element(p)) {
            if (match_selector_from(p, sel, idx - 1, doc)) return true;
        }
        return false;
    }
    return false;
}

bool media_matches(const std::string& prelude, int vw, int vh) {
    std::string q = util::to_lower(util::trim(prelude));
    if (q.empty()) return true;
    auto queries = split_top(q, ',');
    for (auto& query : queries) {
        std::string s = util::trim(query);
        bool negate = false;
        if (util::starts_with(s, "not ")) { negate = true; s = util::trim(s.substr(4)); }
        bool ok = true;
        auto parts = split_top(s, ',');
        (void)parts;
        // split on " and "
        std::vector<std::string> conds;
        std::string cur;
        for (size_t i = 0; i < s.size();) {
            if (s.compare(i, 5, " and ") == 0) { conds.push_back(util::trim(cur)); cur.clear(); i += 5; }
            else { cur.push_back(s[i]); i++; }
        }
        conds.push_back(util::trim(cur));
        for (auto& c : conds) {
            if (c.empty()) continue;
            if (c == "screen" || c == "all" || c == "only screen") continue;
            if (c == "print" || c == "speech" || c == "tv" || c == "projection") { ok = false; break; }
            if (util::starts_with(c, "(") && util::ends_with(c, ")")) {
                std::string feat = util::trim(c.substr(1, c.size() - 2));
                size_t colon = feat.find(':');
                std::string name = util::trim(colon == std::string::npos ? feat : feat.substr(0, colon));
                std::string val = util::trim(colon == std::string::npos ? "" : feat.substr(colon + 1));
                auto pxval = [&](float def) -> float {
                    if (val.empty()) return def;
                    try { return (float)std::atof(val.c_str()); } catch (...) { return def; }
                };
                if (name == "min-width") { if (vw < pxval(0)) ok = false; }
                else if (name == "max-width") { if (vw > pxval(100000)) ok = false; }
                else if (name == "min-height") { if (vh < pxval(0)) ok = false; }
                else if (name == "max-height") { if (vh > pxval(100000)) ok = false; }
                else if (name == "orientation") {
                    bool landscape = vw >= vh;
                    if ((val == "landscape") != landscape) ok = false;
                } else if (name == "prefers-color-scheme") {
                    if (val == "dark") ok = false;
                } else if (name == "hover" || name == "pointer" || name == "any-hover" || name == "any-pointer") {
                    if (val == "hover" || val == "fine") ok = false;
                } else {
                    // Unknown media feature: do not match, safer than applying desktop-only rules.
                    ok = false;
                }
            } else {
                ok = false;
            }
            if (!ok) break;
        }
        if (negate) ok = !ok;
        if (ok) return true;
    }
    return false;
}

void parse_rules(const std::string& css, size_t start, size_t end, std::vector<CssRule>& rules,
                 int* order, int vw, int vh) {
    size_t i = start;
    while (i < end) {
        skip_ws(css, i);
        if (i >= end) break;
        if (css[i] == '@') {
            size_t j = i + 1;
            std::string name;
            while (j < end && (std::isalnum((unsigned char)css[j]) || css[j] == '-')) name.push_back(css[j++]);
            name = util::to_lower(name);
            size_t prelude_start = j;
            int par = 0;
            char quote = 0;
            while (j < end) {
                char c = css[j];
                if (quote) { if (c == '\\') j++; else if (c == quote) quote = 0; j++; continue; }
                if (c == '"' || c == '\'') { quote = c; j++; continue; }
                if (c == '(') par++;
                else if (c == ')') par--;
                if (par == 0 && (c == '{' || c == ';')) break;
                j++;
            }
            std::string prelude = util::trim(css.substr(prelude_start, j - prelude_start));
            if (j >= end) break;
            if (css[j] == ';') { i = j + 1; continue; }
            size_t close = find_matching(css, j, '{', '}');
            if (close == std::string::npos) break;
            size_t body_start = j + 1;
            size_t body_end = close;
            if (name == "media") {
                if (media_matches(prelude, vw, vh)) parse_rules(css, body_start, body_end, rules, order, vw, vh);
            } else if (name == "supports" || name == "layer" || name == "container" || name == "scope") {
                parse_rules(css, body_start, body_end, rules, order, vw, vh);
            }
            // font-face, keyframes, page, etc. unsupported: skip
            i = close + 1;
            continue;
        }
        size_t brace = i;
        int par = 0;
        char quote = 0;
        while (brace < end) {
            char c = css[brace];
            if (quote) { if (c == '\\') brace++; else if (c == quote) quote = 0; brace++; continue; }
            if (c == '"' || c == '\'') { quote = c; brace++; continue; }
            if (c == '(') par++;
            else if (c == ')') par--;
            if (par == 0 && c == '{') break;
            if (par == 0 && c == '}') break;
            brace++;
        }
        if (brace >= end || css[brace] != '{') { i = brace + 1; continue; }
        size_t close = find_matching(css, brace, '{', '}');
        if (close == std::string::npos) break;
        std::string selector_text = util::trim(css.substr(i, brace - i));
        std::string body = css.substr(brace + 1, close - brace - 1);
        auto selectors = parse_selector_list_impl(selector_text);
        if (!selectors.empty()) {
            CssRule rule;
            rule.selectors = std::move(selectors);
            rule.order = (*order)++;
            for (auto& decl : split_top(body, ';')) {
                std::string d = util::trim(decl);
                if (d.empty()) continue;
                size_t colon = d.find(':');
                if (colon == std::string::npos) continue;
                std::string name = util::trim(d.substr(0, colon));
                std::string value = util::trim(d.substr(colon + 1));
                if (name.empty() || value.empty()) continue;
                CssDeclaration cd;
                cd.name = util::starts_with(name, "--") ? name : util::to_lower(name);
                cd.value = value;
                std::string lower = util::to_lower(value);
                size_t ip = lower.rfind("!important");
                if (ip != std::string::npos) {
                    std::string after = util::trim(lower.substr(ip + 10));
                    if (after.empty()) {
                        cd.important = true;
                        cd.value = util::trim(value.substr(0, ip));
                    }
                }
                rule.declarations.push_back(std::move(cd));
            }
            if (!rule.declarations.empty()) rules.push_back(std::move(rule));
        }
        i = close + 1;
    }
}

} // namespace

std::vector<Selector> parse_selector_list(const std::string& text) {
    return parse_selector_list_impl(text);
}

bool css_matches_selector(const Node* node, const Selector& sel, const Document* doc) {
    if (!node || sel.parts.empty()) return false;
    return match_selector_from(node, sel, (int)sel.parts.size() - 1, doc);
}

bool css_matches(const Node* node, const std::string& selector, const Document* doc) {
    auto sels = parse_selector_list_impl(selector);
    for (auto& s : sels) if (css_matches_selector(node, s, doc)) return true;
    return false;
}

Stylesheet parse_css(const std::string& css, const std::string& source_url, int viewport_w, int viewport_h,
                     int* rule_order) {
    Stylesheet sheet;
    sheet.source_url = source_url;
    std::string clean = strip_comments(css);
    int local_order = 0;
    parse_rules(clean, 0, clean.size(), sheet.rules, rule_order ? rule_order : &local_order, viewport_w,
                viewport_h);
    return sheet;
}
