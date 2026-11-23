#include "scip/params.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace scip {

namespace {

[[noreturn]] void fail(const ParamSpec& spec, const std::string& what)
{
    throw std::invalid_argument("parameter '" + spec.name + "': " + what);
}

std::string describe(const ParamValue& value)
{
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return "boolean";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return "integer";
            } else if constexpr (std::is_same_v<T, double>) {
                return "number";
            } else {
                return "string";
            }
        },
        value);
}

void checkRange(const ParamSpec& spec, double value)
{
    if (!spec.hasRange()) {
        return;
    }
    if (value < spec.min || value > spec.max) {
        std::ostringstream os;
        os << formatValue(spec.type == ParamType::Int ? ParamValue(std::int64_t(value))
                                                      : ParamValue(value))
           << " is outside [" << spec.min << ", " << spec.max << "]";
        fail(spec, os.str());
    }
}

}  // namespace

std::string_view toString(ParamType type)
{
    switch (type) {
        case ParamType::Bool:
            return "bool";
        case ParamType::Int:
            return "int";
        case ParamType::Float:
            return "float";
        case ParamType::String:
            return "string";
        case ParamType::Enum:
            return "enum";
    }
    return "unknown";
}

ParamValue coerce(const ParamSpec& spec, const ParamValue& value)
{
    switch (spec.type) {
        case ParamType::Bool:
            if (const auto* b = std::get_if<bool>(&value)) {
                return *b;
            }
            fail(spec, "expected a boolean, got " + describe(value));

        case ParamType::Int: {
            std::int64_t v = 0;
            if (const auto* i = std::get_if<std::int64_t>(&value)) {
                v = *i;
            } else if (const auto* d = std::get_if<double>(&value)) {
                if (std::trunc(*d) != *d) {
                    fail(spec, "expected an integer, got " + formatValue(value));
                }
                v = static_cast<std::int64_t>(*d);
            } else {
                fail(spec, "expected an integer, got " + describe(value));
            }
            checkRange(spec, static_cast<double>(v));
            if (spec.hasRange() && spec.step > 0.0) {
                const double k = (static_cast<double>(v) - spec.min) / spec.step;
                if (std::trunc(k) != k) {
                    std::ostringstream os;
                    os << v << " is not " << spec.min << " + n * " << spec.step;
                    fail(spec, os.str());
                }
            }
            return v;
        }

        case ParamType::Float: {
            double v = 0.0;
            if (const auto* d = std::get_if<double>(&value)) {
                v = *d;
            } else if (const auto* i = std::get_if<std::int64_t>(&value)) {
                v = static_cast<double>(*i);
            } else {
                fail(spec, "expected a number, got " + describe(value));
            }
            if (!std::isfinite(v)) {
                fail(spec, "must be a finite number");
            }
            checkRange(spec, v);
            return v;
        }

        case ParamType::String:
            if (const auto* s = std::get_if<std::string>(&value)) {
                return *s;
            }
            fail(spec, "expected a string, got " + describe(value));

        case ParamType::Enum: {
            const auto* s = std::get_if<std::string>(&value);
            if (s == nullptr) {
                fail(spec, "expected one of the choices, got " + describe(value));
            }
            if (std::find(spec.choices.begin(), spec.choices.end(), *s) == spec.choices.end()) {
                std::string list;
                for (const auto& c : spec.choices) {
                    list += (list.empty() ? "" : ", ") + c;
                }
                fail(spec, "'" + *s + "' is not one of: " + list);
            }
            return *s;
        }
    }
    fail(spec, "unknown parameter type");
}

ParamValue parseValue(const ParamSpec& spec, std::string_view text)
{
    const std::string s(text);
    switch (spec.type) {
        case ParamType::Bool:
            if (s == "true" || s == "1" || s == "yes" || s == "on") {
                return true;
            }
            if (s == "false" || s == "0" || s == "no" || s == "off") {
                return false;
            }
            fail(spec, "'" + s + "' is not a boolean (true/false)");

        case ParamType::Int:
        case ParamType::Float: {
            // strtod for both: accepts "5", "-3", "1.5", "1e3" (GCC 10 has no
            // floating-point std::from_chars).
            errno = 0;
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            if (s.empty() || end != s.c_str() + s.size() || errno == ERANGE) {
                fail(spec, "'" + s + "' is not a number");
            }
            return coerce(spec, ParamValue(v));  // for Int, coerce() rejects fractions
        }

        case ParamType::String:
        case ParamType::Enum:
            return coerce(spec, ParamValue(s));
    }
    fail(spec, "unknown parameter type");
}

std::string formatValue(const ParamValue& value)
{
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                std::ostringstream os;
                os << v;
                return os.str();
            } else {
                return v;
            }
        },
        value);
}

ParamMap resolveParams(const std::vector<ParamSpec>& specs, const ParamMap& given)
{
    ParamMap result;
    for (const ParamSpec& spec : specs) {
        result[spec.name] = coerce(spec, spec.defaultValue);
    }
    for (const auto& [name, value] : given) {
        const auto it = std::find_if(
            specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.name == name; });
        if (it == specs.end()) {
            std::string list;
            for (const auto& s : specs) {
                list += (list.empty() ? "" : ", ") + s.name;
            }
            throw std::invalid_argument("unknown parameter '" + name + "'" +
                                        (list.empty() ? " (takes none)" : " (has: " + list + ")"));
        }
        result[name] = coerce(*it, value);
    }
    return result;
}

bool getBool(const ParamMap& params, const std::string& name)
{
    return std::get<bool>(params.at(name));
}

std::int64_t getInt(const ParamMap& params, const std::string& name)
{
    return std::get<std::int64_t>(params.at(name));
}

double getFloat(const ParamMap& params, const std::string& name)
{
    return std::get<double>(params.at(name));
}

const std::string& getString(const ParamMap& params, const std::string& name)
{
    return std::get<std::string>(params.at(name));
}

}  // namespace scip
