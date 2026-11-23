#include "scip/cv.hpp"
#include "scip/io.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <stdexcept>

namespace scip::io {

Frame readImage(const std::string& path)
{
    const cv::Mat image = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (image.empty()) {
        throw std::runtime_error("cannot read image '" + path + "'");
    }
    if (image.depth() != CV_8U) {
        throw std::runtime_error("'" + path + "': only 8-bit images are supported");
    }

    switch (image.channels()) {
        case 1:
        case 3:
            return toFrame(image);
        case 4: {
            cv::Mat bgr;
            cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
            return toFrame(bgr);
        }
        default:
            throw std::runtime_error("'" + path + "': unsupported channel count");
    }
}

void writeImage(const std::string& path, const Frame& frame)
{
    if (frame.empty()) {
        throw std::invalid_argument("writeImage: empty frame");
    }
    if (!cv::imwrite(path, asMat(frame))) {
        throw std::runtime_error("cannot write image '" + path + "'");
    }
}

}  // namespace scip::io
