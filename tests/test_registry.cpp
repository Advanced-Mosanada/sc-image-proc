#include "scip/registry.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

using scip::Frame;
using scip::FrameInfo;
using scip::ParamSpec;
using scip::ParamType;
using scip::StageInfo;
using scip::StageRegistry;
using scip::StageSpec;

namespace {

/// Test stage: adds `amount` to every byte (saturating).
class AddStage final : public scip::Stage {
public:
    explicit AddStage(int amount) : amount_(amount) {}
    FrameInfo configure(const FrameInfo& input) override { return input; }
    void process(const Frame& in, Frame& out) override
    {
        for (int y = 0; y < in.height(); ++y) {
            for (std::size_t x = 0; x < in.info().rowBytes(); ++x) {
                out.row(y)[x] = static_cast<std::uint8_t>(std::min(255, in.row(y)[x] + amount_));
            }
        }
    }

private:
    int amount_;
};

void fill(StageRegistry& registry)
{
    StageInfo add;
    add.name = "add";
    add.title = "Add constant";
    ParamSpec amount;
    amount.name = "amount";
    amount.type = ParamType::Int;
    amount.defaultValue = std::int64_t{1};
    amount.min = 0;
    amount.max = 255;
    add.params = {amount};
    ParamSpec label;
    label.name = "label";
    label.type = ParamType::String;
    label.defaultValue = std::string("x");
    add.params.push_back(label);
    add.create = [](const scip::ParamMap& p) {
        return std::make_unique<AddStage>(static_cast<int>(scip::getInt(p, "amount")));
    };
    registry.add(add);

    StageInfo none;
    none.name = "none";
    none.create = [](const scip::ParamMap&) { return std::make_unique<AddStage>(0); };
    registry.add(none);
}

}  // namespace

TEST(Registry, ListsAndDescribes)
{
    StageRegistry registry;
    fill(registry);
    const auto all = registry.list();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].name, "add");  // sorted
    EXPECT_TRUE(registry.contains("none"));
    EXPECT_NE(registry.help().find("amount=<int>  default 1, 0..255"), std::string::npos)
        << registry.help();
}

TEST(Registry, RejectsDuplicates)
{
    StageRegistry registry;
    fill(registry);
    StageInfo again = registry.info("add");
    EXPECT_FALSE(registry.add(again));
}

TEST(Registry, CreatesWithDefaultsAndLabels)
{
    StageRegistry registry;
    fill(registry);
    auto stage = registry.create(StageSpec{"add", {{"amount", std::int64_t{10}}}});
    EXPECT_EQ(stage->name(), "add amount=10");

    Frame in(FrameInfo{4, 2, scip::PixelFormat::Gray8});
    in.fill(250);
    Frame out(stage->configure(in.info()));
    stage->process(in, out);
    EXPECT_EQ(out.row(1)[3], 255);
}

TEST(Registry, ParsesChains)
{
    StageRegistry registry;
    fill(registry);
    const auto chain =
        registry.parseChain("add amount=3 ! none !add label=\"a b \\\"c\\\"\" amount=7");
    ASSERT_EQ(chain.size(), 3u);
    EXPECT_EQ(scip::getInt(chain[0].params, "amount"), 3);
    EXPECT_TRUE(chain[1].params.empty());
    EXPECT_EQ(scip::getString(chain[2].params, "label"), "a b \"c\"");
    // Formatting quotes values that need it, and parses back to the same chain.
    const std::string text = scip::formatChain(chain);
    EXPECT_EQ(text, "add amount=3 ! none ! add amount=7 label=\"a b \\\"c\\\"\"");
    EXPECT_EQ(scip::formatChain(registry.parseChain(text)), text);
    EXPECT_TRUE(registry.parseChain("  ").empty());
}

TEST(Registry, ReportsBadChains)
{
    StageRegistry registry;
    fill(registry);
    EXPECT_THROW(registry.parseChain("nope"), std::invalid_argument);
    EXPECT_THROW(registry.parseChain("add ! ! none"), std::invalid_argument);
    EXPECT_THROW(registry.parseChain("add amount"), std::invalid_argument);
    EXPECT_THROW(registry.parseChain("add amount=300"), std::invalid_argument);
    EXPECT_THROW(registry.parseChain("add size=3"), std::invalid_argument);
    EXPECT_THROW(registry.parseChain("add label=\"open"), std::invalid_argument);
    try {
        registry.parseChain("add amount=abc");
        FAIL();
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("add: parameter 'amount'"), std::string::npos)
            << e.what();
    }
}
