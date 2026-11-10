#pragma once
#include "dom.h"
#include <memory>
#include <string>
#include <vector>

struct Selector;

struct AttrSelector {
    std::string name;
    std::string op;   // "", "=", "~=", "^=", "$=", "*=", "|="
    std::string value;
};

struct CompoundSelector {
    std::string tag;   // empty or "*" = any
    std::string id;
    std::vector<std::string> classes;
    std::vector<AttrSelector> attrs;
    std::vector<std::string> pseudos;          // simple pseudo classes
    std::vector<std::pair<std::string, std::string>> pseudo_args; // e.g. nth-child(2n+1)
    std::vector<std::shared_ptr<Selector>> nots;
    std::vector<std::shared_ptr<Selector>> anys;
    bool has_pseudo_element = false;
};

struct SelectorPart {
    CompoundSelector compound;
};

struct Selector {
    std::vector<SelectorPart> parts;      // left to right
    std::vector<char> combinators;        // between parts[i] and parts[i+1]: ' ', '>', '+', '~'
    int spec_id = 0, spec_class = 0, spec_tag = 0;
};

struct CssDeclaration {
    std::string name;
    std::string value;
    bool important = false;
};

struct CssRule {
    std::vector<Selector> selectors;
    std::vector<CssDeclaration> declarations;
    int order = 0;
};

struct Stylesheet {
    std::vector<CssRule> rules;
    std::string source_url;
};

Stylesheet parse_css(const std::string& css, const std::string& source_url, int viewport_w, int viewport_h,
                     int* rule_order);
bool css_matches(const Node* node, const std::string& selector, const Document* doc);
bool css_matches_selector(const Node* node, const Selector& selector, const Document* doc);
std::vector<Selector> parse_selector_list(const std::string& text);

void apply_cascade(Document& doc, const std::vector<Stylesheet>& sheets, int viewport_w, int viewport_h);

extern const char* kUserAgentCss;
