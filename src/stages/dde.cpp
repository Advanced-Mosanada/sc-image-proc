#include "common.hpp"
#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace scip::stages {

namespace {

/// CLAHE (contrast limited adaptive histogram equalisation) for a smooth image
/// kept in float. The tile histograms and their clip limit follow cv::CLAHE
/// (so `clip` and `tiles` mean the same), but the mapping is interpolated
/// between levels as well as between tiles.
///
/// Why: on an 8-bit image CLAHE turns every 1-level step into a step of up to
/// `clip` levels. Noise normally hides that; a denoised base layer has none,
/// and slow gradients (a dark sky) come out as visible contour bands. Here a
/// base value of 20.4 maps 40 % of the way from the mapping of 20 to that of 21.
class SmoothClahe {
public:
    /// Builds the tile mappings from the histograms of `base8` (the base, rounded).
    void build(const cv::Mat& base8, double clip, int tiles)
    {
        tiles_ = tiles;
        tileW_ = (base8.cols + tiles - 1) / tiles;
        tileH_ = (base8.rows + tiles - 1) / tiles;
        prepareColumns(base8.cols);
        histograms_.assign(std::size_t(tiles * tiles) * 256, 0);
        lut_.assign(std::size_t(tiles * tiles) * 257, 0.0f);
        const int cols = base8.cols;
        const int* bin = histogramColumn_.data();
        for (int y = 0; y < base8.rows; ++y) {
            const std::uint8_t* row = base8.ptr<std::uint8_t>(y);
            int* tileRow = &histograms_[std::size_t(std::min(y / tileH_, tiles - 1) * tiles) * 256];
            for (int x = 0; x < cols; ++x) {
                ++tileRow[bin[x] + row[x]];
            }
        }
        for (int t = 0; t < tiles * tiles; ++t) {
            int* h = &histograms_[std::size_t(t) * 256];
            int area = 0;
            for (int i = 0; i < 256; ++i) {
                area += h[i];
            }
            if (area == 0) {
                continue;
            }
            if (clip > 0.0) {  // clip and redistribute exactly as cv::CLAHE does
                const int limit = std::max(int(clip * area / 256), 1);
                int clipped = 0;
                for (int i = 0; i < 256; ++i) {
                    if (h[i] > limit) {
                        clipped += h[i] - limit;
                        h[i] = limit;
                    }
                }
                const int batch = clipped / 256;
                int residual = clipped - batch * 256;
                for (int i = 0; i < 256; ++i) {
                    h[i] += batch;
                }
                if (residual != 0) {
                    const int step = std::max(256 / residual, 1);
                    for (int i = 0; i < 256 && residual > 0; i += step, --residual) {
                        ++h[i];
                    }
                }
            }
            // Level i maps to the middle of its share of the cumulative
            // histogram, so the mapping is continuous between levels.
            float* lut = &lut_[std::size_t(t) * 257];
            const float scale = 255.0f / float(area);
            int sum = 0;
            for (int i = 0; i < 256; ++i) {
                lut[i] = (float(sum) + 0.5f * float(h[i])) * scale;
                sum += h[i];
            }
            lut[256] = lut[255];  // so that level 255 + fraction can be read
        }
    }

    /// Maps one row (image row `y`) of float values in 0..255.
    void mapRow(int y, const float* values, float* out, int cols)
    {
        // Between the centres of the tile rows above and below (bilinear, as cv::CLAHE).
        const float ty = (float(y) + 0.5f) / float(tileH_) - 0.5f;
        const float tyFloor = std::floor(ty);
        const int ty1 = std::max(int(tyFloor), 0);
        const int ty2 = std::min(int(tyFloor) + 1, tiles_ - 1);
        const float ya = ty - tyFloor;  // irrelevant at the edges, where ty1 == ty2
        const float* top = &lut_[std::size_t(ty1 * tiles_) * 257];
        const float* bottom = &lut_[std::size_t(ty2 * tiles_) * 257];
        for (int x = 0; x < cols; ++x) {
            const float v = std::min(std::max(values[x], 0.0f), 255.0f);
            const int i = int(v);
            const float f = v - float(i);
            const Column& c = column_[std::size_t(x)];
            const float* l11 = top + c.left;
            const float* l12 = top + c.right;
            const float* l21 = bottom + c.left;
            const float* l22 = bottom + c.right;
            const float m11 = l11[i] + f * (l11[i + 1] - l11[i]);
            const float m12 = l12[i] + f * (l12[i + 1] - l12[i]);
            const float m21 = l21[i] + f * (l21[i + 1] - l21[i]);
            const float m22 = l22[i] + f * (l22[i + 1] - l22[i]);
            const float upper = m11 + c.weight * (m12 - m11);
            const float lower = m21 + c.weight * (m22 - m21);
            out[x] = upper + ya * (lower - upper);
        }
    }

private:
    struct Column {
        int left;      // offset of the left tile's table in a row of tiles
        int right;     // offset of the right tile's table
        float weight;  // of the right tile
    };

    /// Per column: its histogram (tile) offset, and the two tiles it is
    /// interpolated between, so the per-pixel loops have no divisions.
    void prepareColumns(int cols)
    {
        column_.resize(std::size_t(cols));
        histogramColumn_.resize(std::size_t(cols));
        for (int x = 0; x < cols; ++x) {
            histogramColumn_[std::size_t(x)] = std::min(x / tileW_, tiles_ - 1) * 256;
            const float tx = (float(x) + 0.5f) / float(tileW_) - 0.5f;
            const float txFloor = std::floor(tx);
            const int t1 = std::max(int(txFloor), 0);
            const int t2 = std::min(int(txFloor) + 1, tiles_ - 1);
            column_[std::size_t(x)] = Column{t1 * 257, t2 * 257, tx - txFloor};
        }
    }

    int tiles_ = 1;
    int tileW_ = 1;
    int tileH_ = 1;
    std::vector<int> histograms_;  // per tile: 256 counts
    std::vector<float> lut_;       // per tile: 257 entries
    std::vector<Column> column_;
    std::vector<int> histogramColumn_;
};

/// Digital detail enhancement, the classic way to display thermal (IR) video
/// (FLIR "DDE"; Branchitta et al. 2008, Liu & Zhao 2017 "GF&DDE"): split the
/// image into layers, treat each for what it carries, add them back.
///
///   base = guided filter of I      large areas and the strong edges (float)
///   mid  = blur3x3(I) - base       texture, the detail of homogeneous areas
///   fine = I - blur3x3(I)          finest detail, and most of the pixel noise
///   out  = base + contrast * (CLAHE(base) - base)
///          + boost(gain) * core(mid) + boost(fine_gain) * core(fine)
///
/// - The contrast of the scene is raised on the base only: CLAHE does not see
///   the pixel noise, so it does not amplify it. The base stays in float and
///   CLAHE is interpolated between levels (SmoothClahe), so slow gradients
///   are not cut into bands.
/// - "Coring": a band value d is first reduced to d * d^2 / (d^2 + t^2), so
///   differences at the noise level t are mostly removed and flat areas come
///   out smoother than they went in. t is the band's share of the `noise`
///   level (the input noise sigma): white noise sigma s leaves 0.8 s in the
///   fine band and 0.375 s in the mid band.
/// - The boost is edge-aware: it is 1 + (gain - 1) * (1 - a), with a the
///   guided filter's coefficient (~1 near edges stronger than sqrt(eps)*255,
///   ~0 in flat and textured areas). The base follows a strong edge only to
///   ~90 %; without this the rest of the step would be boosted into a wide
///   halo. Strong edges keep their own contrast and do not clip.
///
/// With gain = fine_gain = 1, noise = 0 and contrast = 0 the output is the input.
class Dde final : public Stage {
public:
    Dde(int radius, double eps, double contrast, double clip, int tiles, double gain,
        double fineGain, double noise)
        : radius_(radius), eps_(eps), contrast_(float(contrast)), clip_(clip), tiles_(tiles),
          gain_(float(gain)), fineGain_(float(fineGain)),
          // Coring thresholds squared; a tiny floor keeps 0 / 0 out when noise = 0.
          midT2_(float(std::max(0.375 * noise * 0.375 * noise, 1e-6))),
          fineT2_(float(std::max(0.8 * noise * 0.8 * noise, 1e-6)))
    {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        luma_.apply(asMat(in), dst, [this](const cv::Mat& src, cv::Mat& result) {
            result.create(src.size(), CV_8U);
            enhance(src, result);
        });
    }

private:
    void enhance(const cv::Mat& src, cv::Mat& dst)
    {
        base8_.create(src.size(), CV_8U);
        filter_.apply(src, base8_, radius_, eps_);
        const bool equalise = contrast_ > 0.0f;
        if (equalise) {
            clahe_.build(base8_, clip_, tiles_);
        }
        cv::GaussianBlur(src, smooth_, cv::Size(3, 3), 0.0, 0.0, cv::BORDER_REFLECT_101);
        base_.create(1, src.cols, CV_32F);
        mapped_.create(1, src.cols, CV_32F);
        // Settings and sizes as locals: writes through uint8_t* may alias
        // members, which would keep the compiler from vectorising the loops.
        const int cols = src.cols;
        const float contrast = contrast_;
        const float midBoost = gain_ - 1.0f;
        const float fineBoost = fineGain_ - 1.0f;
        const float midT2 = midT2_;
        const float fineT2 = fineT2_;
        float* base = base_.ptr<float>();
        float* mapped = mapped_.ptr<float>();
        for (int y = 0; y < src.rows; ++y) {
            const std::uint8_t* i = src.ptr<std::uint8_t>(y);
            const std::uint8_t* s = smooth_.ptr<std::uint8_t>(y);
            const float* a = filter_.coefficient().ptr<float>(y);
            const float* b = filter_.offset().ptr<float>(y);
            std::uint8_t* o = dst.ptr<std::uint8_t>(y);
            for (int x = 0; x < cols; ++x) {
                base[x] = a[x] * float(i[x]) + b[x];  // the base layer, unrounded
            }
            if (equalise) {
                clahe_.mapRow(y, base, mapped, cols);
            } else {
                std::copy(base, base + cols, mapped);
            }
            for (int x = 0; x < cols; ++x) {
                // a = var / (var + eps) is in [0, 1) (box filters and bilinear
                // scaling keep it there), so 1 - a needs no clamp (which would
                // also stop the loop from vectorising).
                const float flat = 1.0f - a[x];  // ~0 at strong edges, ~1 in flat areas
                const float layer = base[x] + contrast * (mapped[x] - base[x]);
                const float mid = float(s[x]) - base[x];
                const float fine = float(i[x]) - float(s[x]);
                const float mid2 = mid * mid;
                const float fine2 = fine * fine;
                const float v = layer + mid * mid2 / (mid2 + midT2) * (1.0f + midBoost * flat) +
                                fine * fine2 / (fine2 + fineT2) * (1.0f + fineBoost * flat);
                o[x] = std::uint8_t(std::min(std::max(v + 0.5f, 0.0f), 255.0f));
            }
        }
    }

    int radius_;
    double eps_;
    float contrast_;
    double clip_;
    int tiles_;
    float gain_;
    float fineGain_;
    float midT2_;
    float fineT2_;
    SmoothClahe clahe_;
    GuidedFilter filter_;
    LumaProcessor luma_;
    cv::Mat base8_, smooth_, base_, mapped_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "dde";
    info.title = "Detail enhancement (thermal DDE)";
    info.description =
        "Base / detail split: local contrast on the base (CLAHE, no noise boost), texture "
        "and fine detail boosted above the noise level, no halos at strong edges";
    info.category = "contrast";
    info.params = {
        param::integer("radius", "Size of the base layer's areas", 8, 1, 32, 0, "px"),
        param::number("eps",
                      "Edges stronger than ~sqrt(eps)*255 stay in the base and are not boosted",
                      0.01,
                      0.0001,
                      0.2,
                      0.0005),
        param::number("contrast", "Local contrast of the base (0 = none, 1 = full CLAHE)", 1.0, 0.0, 1.0, 0.05),
        param::number("clip", "CLAHE contrast limit of the base", 2.0, 0.1, 40.0, 0.1),
        param::integer("tiles", "CLAHE tiles per side (1 = global plateau equalisation)", 8, 1, 32),
        param::number("gain", "Boost of texture / homogeneous-area detail", 2.0, 0.0, 8.0, 0.1),
        param::number("fine_gain", "Boost of the finest detail (pixel level)", 1.0, 0.0, 4.0, 0.1),
        param::number("noise", "Input noise level: smaller differences are not boosted", 2.5, 0.0, 30.0, 0.1),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Dde>(static_cast<int>(getInt(p, "radius")),
                                     getFloat(p, "eps"),
                                     getFloat(p, "contrast"),
                                     getFloat(p, "clip"),
                                     static_cast<int>(getInt(p, "tiles")),
                                     getFloat(p, "gain"),
                                     getFloat(p, "fine_gain"),
                                     getFloat(p, "noise"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages
