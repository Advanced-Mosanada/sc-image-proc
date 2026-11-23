#pragma once

// Zero-copy bridge between scip::Frame and cv::Mat.
// Header-only; include it only from code that links OpenCV.

#include "scip/frame.hpp"

#include <opencv2/core.hpp>

#include <stdexcept>

namespace scip {

inline int cvType(PixelFormat format)
{
    switch (format) {
        case PixelFormat::Gray8:
            return CV_8UC1;
        case PixelFormat::Bgr8:
            return CV_8UC3;
    }
    throw std::invalid_argument("cvType: unsupported pixel format");
}

/// View a frame as a cv::Mat. No copy: the Mat shares the frame's pixels.
inline cv::Mat asMat(Frame& frame)
{
    return cv::Mat(
        frame.height(), frame.width(), cvType(frame.format()), frame.data(), frame.stride());
}

/// Read-only view. cv::Mat has no const-correct variant: never write through it.
inline cv::Mat asMat(const Frame& frame)
{
    return asMat(const_cast<Frame&>(frame));
}

/// Copy an 8-bit, 1- or 3-channel cv::Mat into a newly allocated Frame.
inline Frame toFrame(const cv::Mat& mat)
{
    PixelFormat format{};
    if (mat.type() == CV_8UC1) {
        format = PixelFormat::Gray8;
    } else if (mat.type() == CV_8UC3) {
        format = PixelFormat::Bgr8;
    } else {
        throw std::invalid_argument("toFrame: only CV_8UC1 and CV_8UC3 are supported");
    }
    Frame frame(FrameInfo{mat.cols, mat.rows, format});
    cv::Mat view = asMat(frame);
    mat.copyTo(view);  // same size and type: copies into the frame, no reallocation
    return frame;
}

}  // namespace scip
