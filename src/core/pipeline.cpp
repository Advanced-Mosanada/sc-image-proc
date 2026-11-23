#include "scip/pipeline.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace scip {

namespace {

using Clock = std::chrono::steady_clock;

std::string describe(const FrameInfo& info)
{
    return std::to_string(info.width) + "x" + std::to_string(info.height) + " " +
           std::string(toString(info.format));
}

}  // namespace

void Pipeline::add(StagePtr stage)
{
    if (!stage) {
        throw std::invalid_argument("Pipeline::add: null stage");
    }
    slots_.push_back(Slot{std::move(stage), Frame{}, LatencyStats{}});
    configured_ = false;
}

void Pipeline::configure(const FrameInfo& input)
{
    FrameInfo current = input;
    for (Slot& slot : slots_) {
        try {
            current = slot.stage->configure(current);
        } catch (const std::exception& e) {
            throw std::invalid_argument("stage '" + slot.stage->name() + "' rejected input " +
                                        describe(current) + ": " + e.what());
        }
        slot.output = Frame(current);
        slot.stats.reset();
    }
    inputInfo_ = input;
    outputInfo_ = current;
    total_.reset();
    configured_ = true;
}

const Frame& Pipeline::process(const Frame& input)
{
    if (!configured_) {
        throw std::logic_error("Pipeline::process called before configure()");
    }
    if (input.info() != inputInfo_) {
        throw std::invalid_argument("Pipeline::process: got " + describe(input.info()) +
                                    ", configured for " + describe(inputInfo_));
    }

    const auto start = Clock::now();
    const Frame* current = &input;
    for (Slot& slot : slots_) {
        const auto stageStart = Clock::now();
        slot.stage->process(*current, slot.output);
        slot.stats.add(Clock::now() - stageStart);

        slot.output.meta = current->meta;
        current = &slot.output;
    }
    total_.add(Clock::now() - start);
    return *current;
}

void Pipeline::resetStats()
{
    for (Slot& slot : slots_) {
        slot.stats.reset();
    }
    total_.reset();
}

}  // namespace scip
