// image_compress.cpp
// Build example: g++ -O3 image_compress.cpp lodepng.cpp -o imgc
// Requires: stb_image.h, stb_image_write.h, lodepng.h, lodepng.cpp

#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdint>
#include <cstdlib>   // strtof
#include <limits>
#include <unordered_map>
#include <set>
#include <cctype>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include "lodepng.h"  // for PNG-8 (indexed) output

struct RGB { uint8_t r, g, b; };

// ---------- YCbCr helpers ----------
struct YCbCr {
    float y, cb, cr;

    static YCbCr fromRGB(const RGB& rgb) {
        YCbCr c;
        c.y  = 0.299f * rgb.r + 0.587f * rgb.g + 0.114f * rgb.b;
        c.cb = 128.0f - 0.168736f * rgb.r - 0.331264f * rgb.g + 0.5f * rgb.b;
        c.cr = 128.0f + 0.5f * rgb.r - 0.418688f * rgb.g - 0.081312f * rgb.b;
        return c;
    }

    RGB toRGB() const {
        float r = y + 1.402f * (cr - 128.0f);
        float g = y - 0.344136f * (cb - 128.0f) - 0.714136f * (cr - 128.0f);
        float b = y + 1.772f * (cb - 128.0f);
        RGB out;
        out.r = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(r)), 0, 255));
        out.g = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(g)), 0, 255));
        out.b = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(b)), 0, 255));
        return out;
    }

    RGB toRGBRounded(int multiple = 2) const {
        auto roundToMultiple = [](float val, int mult) -> uint8_t {
            int v = static_cast<int>(std::lround(val / mult) * mult);
            return static_cast<uint8_t>(std::clamp(v, 0, 255));
        };
        float r = y + 1.402f * (cr - 128.0f);
        float g = y - 0.344136f * (cb - 128.0f) - 0.714136f * (cr - 128.0f);
        float b = y + 1.772f * (cb - 128.0f);
        RGB out;
        out.r = roundToMultiple(r, multiple);
        out.g = roundToMultiple(g, multiple);
        out.b = roundToMultiple(b, multiple);
        return out;
    }
};

// ---------- core processing ----------
static inline float quantize(float value, int levels) {
    levels = std::max(levels, 2);
    const float step = 255.0f / (levels - 1);
    return std::round(value / step) * step;
}

static inline float orderedDither(float value, int x, int y, int levels) {
    static constexpr float bayer[4][4] = {
        {0.0f/16, 8.0f/16, 2.0f/16, 10.0f/16},
        {12.0f/16, 4.0f/16, 14.0f/16, 6.0f/16},
        {3.0f/16, 11.0f/16, 1.0f/16, 9.0f/16},
        {15.0f/16, 7.0f/16, 13.0f/16, 5.0f/16}
    };
    const float step = 255.0f / (std::max(levels, 2) - 1);
    const float threshold = (bayer[y % 4][x % 4] - 0.5f) * step;
    const float out = value + threshold;
    return std::clamp(out, 0.0f, 255.0f);
}

void chromaBlur(std::vector<YCbCr>& px, int w, int h, float sigma) {
    if (sigma < 0.1f) return;
    const int radius = static_cast<int>(std::ceil(sigma * 2));
    std::vector<float> kernel(radius * 2 + 1);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        float v = std::exp(-(i * i) / (2.0f * sigma * sigma));
        kernel[i + radius] = v; sum += v;
    }
    for (auto& k : kernel) k /= sum;

    std::vector<YCbCr> tmp = px;
    // horizontal
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float cb = 0.0f, cr = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                int sx = std::clamp(x + i, 0, w - 1);
                float k = kernel[i + radius];
                const auto& s = tmp[y * w + sx];
                cb += s.cb * k; cr += s.cr * k;
            }
            auto& d = px[y * w + x];
            d.cb = cb; d.cr = cr;
        }
    }
    // vertical
    tmp = px;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float cb = 0.0f, cr = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                int sy = std::clamp(y + i, 0, h - 1);
                float k = kernel[i + radius];
                const auto& s = tmp[sy * w + x];
                cb += s.cb * k; cr += s.cr * k;
            }
            auto& d = px[y * w + x];
            d.cb = cb; d.cr = cr;
        }
    }
}

void chromaSubsample(std::vector<YCbCr>& px, int w, int h, int factor) {
    if (factor <= 1) return;
    for (int y = 0; y < h; y += factor) {
        for (int x = 0; x < w; x += factor) {
            float avgCb = 0.0f, avgCr = 0.0f;
            int cnt = 0;
            for (int dy = 0; dy < factor && y + dy < h; ++dy) {
                for (int dx = 0; dx < factor && x + dx < w; ++dx) {
                    const auto& p = px[(y + dy) * w + (x + dx)];
                    avgCb += p.cb; avgCr += p.cr; ++cnt;
                }
            }
            avgCb /= static_cast<float>(cnt);
            avgCr /= static_cast<float>(cnt);
            for (int dy = 0; dy < factor && y + dy < h; ++dy) {
                for (int dx = 0; dx < factor && x + dx < w; ++dx) {
                    auto& p = px[(y + dy) * w + (x + dx)];
                    p.cb = avgCb; p.cr = avgCr;
                }
            }
        }
    }
}

// ---------- PNG-8 helper via lodepng ----------
static bool write_png8_indexed(
    const char* filename,
    const std::vector<uint8_t>& indices,
    const std::vector<uint8_t>& paletteRGBA,
    unsigned w, unsigned h
) {
    std::vector<unsigned char> outPNG;
    lodepng::State state;
    state.info_raw.colortype = LCT_PALETTE;
    state.info_raw.bitdepth  = 8;
    state.info_png.color.colortype = LCT_PALETTE;
    state.info_png.color.bitdepth  = 8;
    state.encoder.auto_convert = 0; // keep palette; no auto truecolor

    const size_t n = paletteRGBA.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        unsigned r = paletteRGBA[i*4 + 0];
        unsigned g = paletteRGBA[i*4 + 1];
        unsigned b = paletteRGBA[i*4 + 2];
        unsigned a = paletteRGBA[i*4 + 3];
        lodepng_palette_add(&state.info_png.color, r, g, b, a);
        lodepng_palette_add(&state.info_raw,       r, g, b, a);
    }

    unsigned err = lodepng::encode(outPNG, indices, w, h, state);
    if (err) {
        std::cerr << "lodepng encode error " << err << ": "
                  << lodepng_error_text(err) << "\n";
        return false;
    }
    err = lodepng::save_file(outPNG, filename);
    if (err) {
        std::cerr << "lodepng save_file error " << err << ": "
                  << lodepng_error_text(err) << "\n";
        return false;
    }
    return true;
}

static inline uint32_t packRGB(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

// ---------- main compression ----------
// NOTE: 'compression' here means QUALITY in [0,1], where 1.0 = highest quality.
bool compressImage(const char* input, const char* output, float compression) {
    if (!(compression >= 0.0f && compression <= 1.0f) || !std::isfinite(compression)) {
        std::cerr << "Compression (quality) must be a finite float in [0.0, 1.0]\n";
        return false;
    }
    const float quality = compression;     // alias for clarity
    const float inv     = 1.0f - quality;  // old "compression" scale

    // detect extension early
    std::string outPath(output);
    std::size_t dotPos = outPath.find_last_of('.');
    if (dotPos == std::string::npos) {
        std::cerr << "Error: Output filename must end with .png or .jpg/.jpeg\n";
        return false;
    }
    std::string ext = outPath.substr(dotPos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    const bool isJPEG = (ext == "jpg" || ext == "jpeg");
    const bool isPNG  = (ext == "png");

    int w = 0, h = 0, src_ch = 0;
    unsigned char* data = stbi_load(input, &w, &h, &src_ch, 3);
    if (!data) {
        std::cerr << "Failed to load image: " << input << "\n";
        return false;
    }
    std::cout << "Loaded " << w << "x" << h << " (source channels: "
              << src_ch << ", working: 3)\n";

    bool ok = false;

    if (isJPEG) {
        std::cout << "Using standard JPEG encoder pipeline.\n";

        // optional light chroma denoise at lower quality (quality <= 0.6)
        if (quality <= 0.6f) {
            std::vector<YCbCr> ycbcr(w * h);
            for (int i = 0; i < w*h; ++i)
                ycbcr[i] = YCbCr::fromRGB({data[i*3], data[i*3+1], data[i*3+2]});
            chromaBlur(ycbcr, w, h, 0.4f);
            for (int i = 0; i < w*h; ++i) {
                RGB rgb = ycbcr[i].toRGB();
                data[i*3] = rgb.r; data[i*3+1] = rgb.g; data[i*3+2] = rgb.b;
            }
        }

        // Map quality [0,1] -> JPEG quality [50..95]
        int jpegQuality = 50 + static_cast<int>(quality * 45.0f);
        jpegQuality = std::clamp(jpegQuality, 1, 100);
        std::cout << "Writing JPEG quality: " << jpegQuality << "\n";
        ok = (stbi_write_jpg(output, w, h, 3, data, jpegQuality) != 0);

    } else if (isPNG) {
        std::cout << "Using custom PNG compression pipeline.\n";

        // 1) RGB -> YCbCr
        std::vector<YCbCr> ycbcr(w * h);
        for (int i = 0; i < w*h; ++i)
            ycbcr[i] = YCbCr::fromRGB({data[i*3], data[i*3+1], data[i*3+2]});

        // 2) params — flip tier logic using 'inv'
        // Old: useTier1 when compression <= 0.3
        // New: useTier1 when inv <= 0.3  => quality >= 0.7
        const bool useTier1 = (quality >= 0.7f - 1e-6f);

        int   lumaLevels, chromaLevels, subsampleFactor;
        float blurSigma; bool useDithering;

        if (useTier1) {
            subsampleFactor = 2;
            float t = inv / 0.3f; // 0..1 as quality drops
            lumaLevels   = 256 - static_cast<int>(t * 64.0f);
            chromaLevels = 256 - static_cast<int>(t * 192.0f);
            blurSigma    = t * 0.7f;
            useDithering = true;
        } else {
            float t = (inv - 0.3f) / 0.7f; // 0..1 as quality gets lower
            t = std::clamp(t, 0.0f, 1.0f);
            lumaLevels       = std::max(4,  192 - static_cast<int>(t * 188.0f));
            chromaLevels     = std::max(2,   64 - static_cast<int>(t *  62.0f));
            subsampleFactor  = 2 + static_cast<int>(t * 6.0f); // up to ~8
            blurSigma        = 0.7f + t * 0.6f;
            useDithering     = (t < 0.5f);
        }

        std::cout << "Quality (0..1): " << quality
                  << (useTier1 ? "  -> Tier 1 (perceptually lossless-ish)\n"
                               : "  -> Tier 2+ (visible compression)\n");
        std::cout << "Luma levels: " << lumaLevels << "\n"
                  << "Chroma levels: " << chromaLevels << "\n"
                  << "Chroma subsample: " << subsampleFactor << "x\n"
                  << "Chroma blur sigma: " << blurSigma << "\n"
                  << "Ordered dithering: " << (useDithering ? "on" : "off") << "\n";

        // 3) blur + subsample
        if (blurSigma > 0.0f) chromaBlur(ycbcr, w, h, blurSigma);
        chromaSubsample(ycbcr, w, h, subsampleFactor);

        // 4) quantize (+ dither Y if enabled)
        if (useDithering) {
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                int idx = y*w + x;
                float dY = orderedDither(ycbcr[idx].y, x, y, lumaLevels);
                ycbcr[idx].y  = quantize(dY,            lumaLevels);
                ycbcr[idx].cb = quantize(ycbcr[idx].cb, chromaLevels);
                ycbcr[idx].cr = quantize(ycbcr[idx].cr, chromaLevels);
            }
        } else {
            for (auto& p : ycbcr) {
                p.y  = quantize(p.y,  lumaLevels);
                p.cb = quantize(p.cb, chromaLevels);
                p.cr = quantize(p.cr, chromaLevels);
            }
        }

        // 5) (optional) even-round Y only if dithering is on
        if (useDithering) {
            for (auto& p : ycbcr) {
                p.y = std::round(p.y / 2.0f) * 2.0f;
                p.y = std::clamp(p.y, 0.0f, 255.0f);
            }
        }

        // 6) back to RGB with perceptual rounding
        // Old threshold: compression < 0.6  -> now quality > 0.4
        const int rgbMultiple = (quality > 0.4f) ? 2 : 4;
        for (int i = 0; i < w*h; ++i) {
            RGB rgb = ycbcr[i].toRGBRounded(rgbMultiple);
            data[i*3] = rgb.r; data[i*3+1] = rgb.g; data[i*3+2] = rgb.b;
        }

        // 7) try PNG-8 (≤256 colors), else PNG-24
        std::set<uint32_t> uniq;
        for (int i = 0; i < w*h; ++i) {
            uniq.insert(packRGB(data[i*3], data[i*3+1], data[i*3+2]));
            if (uniq.size() > 256) break;
        }

        if (!uniq.empty() && uniq.size() <= 256) {
            std::vector<uint8_t> palette; palette.reserve(uniq.size()*4);
            std::unordered_map<uint32_t,uint8_t> toIdx; toIdx.reserve(uniq.size()*2);
            uint8_t idx = 0;
            for (uint32_t c : uniq) {
                palette.push_back((c>>16)&0xFF);
                palette.push_back((c>>8 )&0xFF);
                palette.push_back((c    )&0xFF);
                palette.push_back(255);
                toIdx[c] = idx++;
            }
            std::vector<uint8_t> indices(w*h);
            for (int i = 0; i < w*h; ++i) {
                uint32_t c = packRGB(data[i*3], data[i*3+1], data[i*3+2]);
                indices[i] = toIdx[c];
            }
            std::cout << "Writing PNG-8 (indexed) via lodepng (" << uniq.size() << " colors)\n";
            ok = write_png8_indexed(output, indices, palette, (unsigned)w, (unsigned)h);
            if (!ok) {
                std::cerr << "PNG-8 encode failed. Falling back to PNG-24.\n";
                stbi_write_png_compression_level = 9;
                ok = (stbi_write_png(output, w, h, 3, data, w*3) != 0);
            }
        } else {
            stbi_write_png_compression_level = 9;
            ok = (stbi_write_png(output, w, h, 3, data, w*3) != 0);
            if (ok) std::cout << "Wrote PNG-24 (truecolor)\n";
        }

    } else {
        std::cerr << "Unsupported output format. Use .png or .jpg/.jpeg\n";
    }

    stbi_image_free(data);
    if (!ok) std::cerr << "Failed to write image: " << output << "\n";
    else std::cout << "Compressed image saved to: " << output << "\n";
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cout << "Usage: " << argv[0] << " <input> <output> <compression>\n";
        std::cout << "  input: .png, .jpg, or .jpeg file\n";
        std::cout << "  output: .png or .jpg/.jpeg file\n";
        std::cout << "  compression: 0.0 (lowest quality) to 1.0 (highest quality)\n";
        return 1;
    }

    const char* input  = argv[1];
    const char* output = argv[2];

    char* endp = nullptr;
    float compression = std::strtof(argv[3], &endp);
    if (endp == argv[3] || !std::isfinite(compression) ||
        compression < 0.0f || compression > 1.0f) {
        std::cerr << "compression must be a float in [0.0, 1.0]\n";
        return 1;
    }

    return compressImage(input, output, compression) ? 0 : 1;
}