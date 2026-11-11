#pragma once
#include "dom.h"
#include <string>
#include <vector>

Node* parse_html_document(Document& doc, const std::string& source);
std::vector<Node*> parse_html_fragment(Document& doc, const std::string& source);
