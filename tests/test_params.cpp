#include "scip/params.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using scip::ParamMap;
using scip::ParamSpec;
using scip::ParamType;
using scip::ParamValue;

namespace {

ParamSpec oddKernel()
{
    ParamSpec spec;
    spec.name = "ksize";
    spec.type = ParamType::Int;
    spec.defaultValue = std::int64_t{5};
    spec.min = 3;
    spec.max = 31;
    spec.step = 2;
    return spec;
}

ParamSpec method()
{
    ParamSpec spec;
    spec.name = "method";
    spec.type = ParamType::Enum;
    spec.defaultValue = std::string("none");
    spec.choices = {"none", "clockwise", "horizontal-flip"};
    return spec;
}

}  // namespace

TEST(Params, IntRangeAndStep)
{
    const ParamSpec spec = oddKernel();
    EXPECT_EQ(std::get<std::int64_t>(scip::coerce(spec, std::int64_t{7})), 7);
    EXPECT_EQ(std::get<std::int64_t>(scip::coerce(spec, 9.0)), 9);              // JSON numbers
    EXPECT_THROW(scip::coerce(spec, std::int64_t{4}), std::invalid_argument);   // step
    EXPECT_THROW(scip::coerce(spec, std::int64_t{33}), std::invalid_argument);  // range
    EXPECT_THROW(scip::coerce(spec, 5.5), std::invalid_argument);               // fraction
    EXPECT_THROW(scip::coerce(spec, std::string("5")), std::invalid_argument);  // type
}

TEST(Params, FloatAcceptsIntegers)
{
    ParamSpec spec;
    spec.name = "sigma";
    spec.type = ParamType::Float;
    spec.defaultValue = 0.0;
    EXPECT_DOUBLE_EQ(std::get<double>(scip::coerce(spec, std::int64_t{2})), 2.0);
}

TEST(Params, EnumChoices)
{
    const ParamSpec spec = method();
    EXPECT_EQ(std::get<std::string>(scip::coerce(spec, std::string("clockwise"))), "clockwise");
    EXPECT_THROW(scip::coerce(spec, std::string("sideways")), std::invalid_argument);
}

TEST(Params, ParseFromText)
{
    EXPECT_EQ(std::get<std::int64_t>(scip::parseValue(oddKernel(), "11")), 11);
    EXPECT_THROW(scip::parseValue(oddKernel(), "eleven"), std::invalid_argument);
    EXPECT_THROW(scip::parseValue(oddKernel(), "1.5"), std::invalid_argument);

    ParamSpec flag;
    flag.name = "invert";
    flag.type = ParamType::Bool;
    flag.defaultValue = false;
    EXPECT_TRUE(std::get<bool>(scip::parseValue(flag, "true")));
    EXPECT_FALSE(std::get<bool>(scip::parseValue(flag, "off")));
    EXPECT_THROW(scip::parseValue(flag, "maybe"), std::invalid_argument);
}

TEST(Params, FormatRoundTrips)
{
    EXPECT_EQ(scip::formatValue(true), "true");
    EXPECT_EQ(scip::formatValue(std::int64_t{-3}), "-3");
    EXPECT_EQ(scip::formatValue(1.5), "1.5");
    EXPECT_EQ(scip::formatValue(std::string("clockwise")), "clockwise");
}

TEST(Params, ResolveFillsDefaultsAndRejectsUnknown)
{
    const std::vector<ParamSpec> specs{oddKernel(), method()};
    const ParamMap resolved = scip::resolveParams(specs, {{"ksize", std::int64_t{9}}});
    EXPECT_EQ(scip::getInt(resolved, "ksize"), 9);
    EXPECT_EQ(scip::getString(resolved, "method"), "none");
    EXPECT_THROW(scip::resolveParams(specs, {{"kernel", std::int64_t{9}}}), std::invalid_argument);
}
