#include "scip/registry.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace scip {

namespace {

bool needsQuotes(const std::string& text)
{
    if (text.empty()) {
        return true;
    }
    return std::any_of(text.begin(), text.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) || c == '!' || c == '"' || c == '=';
    });
}

std::string quote(const std::string& text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

/// Splits on `separator` outside double quotes; keeps quotes and escapes.
std::vector<std::string> splitOutsideQuotes(std::string_view text, char separator)
{
    std::vector<std::string> parts(1);
    bool inQuotes = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inQuotes && c == '\\' && i + 1 < text.size()) {
            parts.back() += c;
            parts.back() += text[++i];
            continue;
        }
        if (c == '"') {
            inQuotes = !inQuotes;
        }
        if (!inQuotes && c == separator) {
            parts.emplace_back();
        } else {
            parts.back() += c;
        }
    }
    if (inQuotes) {
        throw std::invalid_argument("unterminated quote in '" + std::string(text) + "'");
    }
    return parts;
}

/// Splits an element into whitespace-separated tokens, removing quotes.
std::vector<std::string> tokenize(const std::string& element)
{
    std::vector<std::string> tokens;
    std::string current;
    bool inToken = false;
    bool inQuotes = false;
    for (std::size_t i = 0; i < element.size(); ++i) {
        const char c = element[i];
        if (inQuotes) {
            if (c == '\\' && i + 1 < element.size()) {
                current += element[++i];
            } else if (c == '"') {
                inQuotes = false;
            } else {
                current += c;
            }
        } else if (c == '"') {
            inQuotes = true;
            inToken = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inToken) {
                tokens.push_back(current);
                current.clear();
                inToken = false;
            }
        } else {
            current += c;
            inToken = true;
        }
    }
    if (inToken) {
        tokens.push_back(current);
    }
    return tokens;
}

const ParamSpec* findParam(const StageInfo& info, const std::string& name)
{
    for (const ParamSpec& spec : info.params) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

}  // namespace

std::string formatSpec(const StageSpec& spec)
{
    std::string text = spec.name;
    for (const auto& [key, value] : spec.params) {
        const std::string formatted = formatValue(value);
        text += " " + key + "=" + (needsQuotes(formatted) ? quote(formatted) : formatted);
    }
    return text;
}

std::string formatChain(const std::vector<StageSpec>& chain)
{
    std::string text;
    for (const StageSpec& spec : chain) {
        text += (text.empty() ? "" : " ! ") + formatSpec(spec);
    }
    return text;
}

StageRegistry& StageRegistry::instance()
{
    static StageRegistry registry;
    return registry;
}

bool StageRegistry::add(StageInfo info)
{
    if (info.name.empty() || !info.create) {
        throw std::invalid_argument("StageRegistry::add: a stage needs a name and a factory");
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    return stages_.emplace(info.name, std::move(info)).second;
}

bool StageRegistry::contains(const std::string& name) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return stages_.count(name) != 0;
}

StageInfo StageRegistry::info(const std::string& name) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = stages_.find(name);
    if (it == stages_.end()) {
        std::string names;
        for (const auto& entry : stages_) {
            names += (names.empty() ? "" : ", ") + entry.first;
        }
        throw std::invalid_argument("unknown function '" + name + "' (available: " + names + ")");
    }
    return it->second;
}

std::vector<StageInfo> StageRegistry::list() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StageInfo> result;
    result.reserve(stages_.size());
    for (const auto& entry : stages_) {
        result.push_back(entry.second);
    }
    return result;  // std::map keeps them sorted by name
}

ParamMap StageRegistry::resolve(const StageSpec& spec) const
{
    const StageInfo stageInfo = info(spec.name);
    try {
        return resolveParams(stageInfo.params, spec.params);
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(spec.name + ": " + e.what());
    }
}

StagePtr StageRegistry::create(const StageSpec& spec) const
{
    const StageInfo stageInfo = info(spec.name);
    const ParamMap params = resolve(spec);
    StagePtr stage;
    try {
        stage = stageInfo.create(params);
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(spec.name + ": " + e.what());
    }
    stage->setLabel(formatSpec(spec));
    return stage;
}

std::vector<StageSpec> StageRegistry::parseChain(std::string_view text) const
{
    std::vector<StageSpec> chain;
    for (const std::string& element : splitOutsideQuotes(text, '!')) {
        const std::vector<std::string> tokens = tokenize(element);
        if (tokens.empty()) {
            if (splitOutsideQuotes(text, '!').size() == 1) {
                return {};  // empty text = empty pipeline
            }
            throw std::invalid_argument("empty element in pipeline '" + std::string(text) + "'");
        }
        StageSpec spec;
        spec.name = tokens[0];
        const StageInfo stageInfo = info(spec.name);
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const auto eq = tokens[i].find('=');
            if (eq == std::string::npos || eq == 0) {
                throw std::invalid_argument(spec.name + ": expected key=value, got '" + tokens[i] +
                                            "'");
            }
            const std::string key = tokens[i].substr(0, eq);
            const ParamSpec* param = findParam(stageInfo, key);
            if (param == nullptr) {
                std::string names;
                for (const ParamSpec& p : stageInfo.params) {
                    names += (names.empty() ? "" : ", ") + p.name;
                }
                throw std::invalid_argument(
                    spec.name + ": unknown parameter '" + key + "'" +
                    (names.empty() ? " (takes none)" : " (has: " + names + ")"));
            }
            try {
                spec.params[key] = parseValue(*param, tokens[i].substr(eq + 1));
            } catch (const std::invalid_argument& e) {
                throw std::invalid_argument(spec.name + ": " + e.what());
            }
        }
        chain.push_back(std::move(spec));
    }
    return chain;
}

std::vector<StagePtr> StageRegistry::createChain(std::string_view text) const
{
    std::vector<StagePtr> stages;
    for (const StageSpec& spec : parseChain(text)) {
        stages.push_back(create(spec));
    }
    return stages;
}

std::string StageRegistry::help() const
{
    std::ostringstream os;
    for (const StageInfo& stageInfo : list()) {
        os << "  " << stageInfo.name << "  -  " << stageInfo.title << " [" << stageInfo.backend
           << "]\n";
        if (!stageInfo.description.empty()) {
            os << "      " << stageInfo.description << "\n";
        }
        for (const ParamSpec& p : stageInfo.params) {
            os << "      " << p.name << "=<" << toString(p.type) << ">  default "
               << formatValue(p.defaultValue);
            if (p.hasRange()) {
                os << ", " << p.min << ".." << p.max;
                if (p.step > 0.0) {
                    os << " step " << p.step;
                }
            }
            if (!p.choices.empty()) {
                os << ", one of:";
                for (const auto& c : p.choices) {
                    os << " " << c;
                }
            }
            if (!p.description.empty()) {
                os << "  -  " << p.description;
            }
            os << "\n";
        }
    }
    return os.str();
}

StageRegistrar::StageRegistrar(StageInfo info)
{
    StageRegistry::instance().add(std::move(info));
}

}  // namespace scip
