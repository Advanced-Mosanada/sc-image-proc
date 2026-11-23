#include "scip/pipeline.hpp"
#include "scip/stages.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

using scip::Frame;
using scip::FrameInfo;
using scip::Pipeline;
using scip::PixelFormat;

namespace {

/// Minimal stage for testing the pipeline itself: out = 255 - in.
class Invert final : public scip::Stage {
public:
    std::string name() const override { return "invert"; }
    FrameInfo configure(const FrameInfo& input) override
    {
        if (input.format != PixelFormat::Gray8) {
            throw std::invalid_argument("gray8 only");
        }
        return input;
    }
    void process(const Frame& in, Frame& out) override
    {
        for (int y = 0; y < in.height(); ++y) {
            for (int x = 0; x < in.width(); ++x) {
                out.row(y)[x] = static_cast<std::uint8_t>(255 - in.row(y)[x]);
            }
        }
    }
};

}  // namespace

TEST(Pipeline, EmptyPipelineReturnsInput)
{
    Pipeline pipeline;
    const FrameInfo info{4, 4, PixelFormat::Gray8};
    pipeline.configure(info);
    Frame input(info);
    EXPECT_EQ(&pipeline.process(input), &input);
}

TEST(Pipeline, ProcessBeforeConfigureThrows)
{
    Pipeline pipeline;
    pipeline.add(std::make_unique<Invert>());
    Frame input(FrameInfo{4, 4, PixelFormat::Gray8});
    EXPECT_THROW(pipeline.process(input), std::logic_error);
}

TEST(Pipeline, RejectsMismatchedInput)
{
    Pipeline pipeline;
    pipeline.add(std::make_unique<Invert>());
    pipeline.configure(FrameInfo{4, 4, PixelFormat::Gray8});
    Frame wrongSize(FrameInfo{8, 4, PixelFormat::Gray8});
    EXPECT_THROW(pipeline.process(wrongSize), std::invalid_argument);
}

TEST(Pipeline, ConfigureReportsWhichStageFailed)
{
    Pipeline pipeline;
    pipeline.add(std::make_unique<Invert>());
    try {
        pipeline.configure(FrameInfo{4, 4, PixelFormat::Bgr8});
        FAIL() << "expected configure() to throw";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("invert"), std::string::npos) << e.what();
    }
}

TEST(Pipeline, ChainsStagesAndPropagatesFormat)
{
    Pipeline pipeline;
    pipeline.add(scip::stages::makeResize(32, 16));
    pipeline.add(scip::stages::makeToGray());
    pipeline.add(std::make_unique<Invert>());
    pipeline.configure(FrameInfo{64, 64, PixelFormat::Bgr8});

    EXPECT_EQ(pipeline.outputInfo(), (FrameInfo{32, 16, PixelFormat::Gray8}));

    Frame input(pipeline.inputInfo());
    input.fill(200);
    input.meta.sequence = 42;
    const Frame& output = pipeline.process(input);

    EXPECT_EQ(output.info(), pipeline.outputInfo());
    EXPECT_EQ(output.row(5)[5], 255 - 200);
    EXPECT_EQ(output.meta.sequence, 42u);
}

TEST(Pipeline, RecordsStatsPerStage)
{
    Pipeline pipeline;
    pipeline.add(std::make_unique<Invert>());
    pipeline.configure(FrameInfo{16, 16, PixelFormat::Gray8});
    Frame input(pipeline.inputInfo());
    input.fill(0);
    for (int i = 0; i < 3; ++i) {
        pipeline.process(input);
    }
    EXPECT_EQ(pipeline.stageStats(0).count(), 3u);
    EXPECT_EQ(pipeline.totalStats().count(), 3u);

    pipeline.resetStats();
    EXPECT_EQ(pipeline.totalStats().count(), 0u);
}
