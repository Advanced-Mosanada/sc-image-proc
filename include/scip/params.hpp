#pragma once

// Self-describing parameters for processing functions.
//
// Every function (stage) publishes a list of ParamSpec. The same description is
// used to validate values, to parse them from pipeline text ("blur ksize=5"), to
// answer the AIPC `functions.list` request and, from that, to build GUI widgets.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scip {

enum class ParamType {
    Bool,
    Int,
    Float,
    String,
    Enum,  ///< one of ParamSpec::choices, stored as a string
};

std::string_view toString(ParamType type);

using ParamValue = std::variant<bool, std::int64_t, double, std::string>;
using ParamMap = std::map<std::string, ParamValue>;

struct ParamSpec {
    std::string name;
    ParamType type = ParamType::Int;
    std::string description;
    ParamValue defaultValue;
    /// Numeric limits. Only enforced when min < max.
    double min = 0.0;
    double max = 0.0;
    /// Numeric step. For Int with min < max, values must be min + k * step
    /// (e.g. min 3, step 2 = odd numbers). 0 = any value.
    double step = 0.0;
    std::vector<std::string> choices;  ///< Enum only
    std::string unit;                  ///< optional, for display ("px", "ms"...)

    bool hasRange() const { return min < max; }
};

/// Check `value` against `spec` and convert it to the spec's type
/// (an integer is accepted for a Float, etc.). Throws std::invalid_argument
/// with a message naming the parameter.
ParamValue coerce(const ParamSpec& spec, const ParamValue& value);

/// Parse text ("5", "1.5", "true", "clockwise") as a value of the spec's type.
ParamValue parseValue(const ParamSpec& spec, std::string_view text);

/// Text form of a value, as parseValue() reads it back.
std::string formatValue(const ParamValue& value);

/// Defaults for all specs, overridden by `given`. Unknown names and invalid
/// values throw std::invalid_argument.
ParamMap resolveParams(const std::vector<ParamSpec>& specs, const ParamMap& given);

/// Builders that keep stage declarations short:
///   param::integer("ksize", "Kernel size", 5, 3, 31, 2, "px")
namespace param {
ParamSpec boolean(std::string name, std::string description, bool defaultValue);
ParamSpec integer(std::string name,
                  std::string description,
                  std::int64_t defaultValue,
                  std::int64_t min,
                  std::int64_t max,
                  std::int64_t step = 0,
                  std::string unit = {});
ParamSpec number(std::string name,
                 std::string description,
                 double defaultValue,
                 double min,
                 double max,
                 double step = 0.0,
                 std::string unit = {});
ParamSpec choice(std::string name,
                 std::string description,
                 std::string defaultValue,
                 std::vector<std::string> choices);
ParamSpec text(std::string name, std::string description, std::string defaultValue);
}  // namespace param

// Typed access to resolved parameters. Throw std::out_of_range if missing.
bool getBool(const ParamMap& params, const std::string& name);
std::int64_t getInt(const ParamMap& params, const std::string& name);
double getFloat(const ParamMap& params, const std::string& name);
const std::string& getString(const ParamMap& params, const std::string& name);

}  // namespace scip
