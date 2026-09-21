module;

#include <cmath>

#if defined(__SSE2__)
#    include <emmintrin.h>
#elif defined(__ARM_NEON)
#    include <arm_neon.h>
#endif

module owe.fft;

import rstd;

namespace owe::fft
{

using namespace rstd::prelude;

namespace
{

static_assert(sizeof(Complex32) == sizeof(float) * 2);

rstd::size_t NextPowerOfTwo(rstd::size_t value) {
    rstd::size_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

Complex32 Multiply(Complex32 left, Complex32 right) {
    return {
        .real = left.real * right.real - left.imag * right.imag,
        .imag = left.real * right.imag + left.imag * right.real,
    };
}

void MultiplyPair(const Complex32* left, const Complex32* right, Complex32* output) {
#if defined(__SSE2__)
    const auto left_values   = _mm_loadu_ps(reinterpret_cast<const float*>(left));
    const auto right_values  = _mm_loadu_ps(reinterpret_cast<const float*>(right));
    const auto left_real     = _mm_shuffle_ps(left_values, left_values, _MM_SHUFFLE(2, 2, 0, 0));
    const auto left_imag     = _mm_shuffle_ps(left_values, left_values, _MM_SHUFFLE(3, 3, 1, 1));
    const auto right_swapped = _mm_shuffle_ps(right_values, right_values, _MM_SHUFFLE(2, 3, 0, 1));
    const auto sign          = _mm_set_ps(0.0f, -0.0f, 0.0f, -0.0f);
    const auto result        = _mm_add_ps(_mm_mul_ps(left_real, right_values),
                                          _mm_xor_ps(_mm_mul_ps(left_imag, right_swapped), sign));
    _mm_storeu_ps(reinterpret_cast<float*>(output), result);
#elif defined(__ARM_NEON)
    const auto    left_values  = vld2_f32(reinterpret_cast<const float*>(left));
    const auto    right_values = vld2_f32(reinterpret_cast<const float*>(right));
    float32x2x2_t result;
    result.val[0] = vmls_f32(
        vmul_f32(left_values.val[0], right_values.val[0]), left_values.val[1], right_values.val[1]);
    result.val[1] = vmla_f32(
        vmul_f32(left_values.val[0], right_values.val[1]), left_values.val[1], right_values.val[0]);
    vst2_f32(reinterpret_cast<float*>(output), result);
#else
    output[0] = Multiply(left[0], right[0]);
    output[1] = Multiply(left[1], right[1]);
#endif
}

void ButterflyPair(Complex32* even, Complex32* odd, const Complex32* weights) {
#if defined(__SSE2__)
    const auto odd_values    = _mm_loadu_ps(reinterpret_cast<const float*>(odd));
    const auto weight_values = _mm_loadu_ps(reinterpret_cast<const float*>(weights));
    const auto odd_real      = _mm_shuffle_ps(odd_values, odd_values, _MM_SHUFFLE(2, 2, 0, 0));
    const auto odd_imag      = _mm_shuffle_ps(odd_values, odd_values, _MM_SHUFFLE(3, 3, 1, 1));
    const auto weights_swapped =
        _mm_shuffle_ps(weight_values, weight_values, _MM_SHUFFLE(2, 3, 0, 1));
    const auto sign        = _mm_set_ps(0.0f, -0.0f, 0.0f, -0.0f);
    const auto product     = _mm_add_ps(_mm_mul_ps(odd_real, weight_values),
                                        _mm_xor_ps(_mm_mul_ps(odd_imag, weights_swapped), sign));
    const auto even_values = _mm_loadu_ps(reinterpret_cast<const float*>(even));
    _mm_storeu_ps(reinterpret_cast<float*>(even), _mm_add_ps(even_values, product));
    _mm_storeu_ps(reinterpret_cast<float*>(odd), _mm_sub_ps(even_values, product));
#elif defined(__ARM_NEON)
    const auto    odd_values    = vld2_f32(reinterpret_cast<const float*>(odd));
    const auto    weight_values = vld2_f32(reinterpret_cast<const float*>(weights));
    float32x2x2_t product;
    product.val[0] = vmls_f32(
        vmul_f32(odd_values.val[0], weight_values.val[0]), odd_values.val[1], weight_values.val[1]);
    product.val[1] = vmla_f32(
        vmul_f32(odd_values.val[0], weight_values.val[1]), odd_values.val[1], weight_values.val[0]);
    const auto    even_values = vld2_f32(reinterpret_cast<const float*>(even));
    float32x2x2_t sum;
    float32x2x2_t difference;
    sum.val[0]        = vadd_f32(even_values.val[0], product.val[0]);
    sum.val[1]        = vadd_f32(even_values.val[1], product.val[1]);
    difference.val[0] = vsub_f32(even_values.val[0], product.val[0]);
    difference.val[1] = vsub_f32(even_values.val[1], product.val[1]);
    vst2_f32(reinterpret_cast<float*>(even), sum);
    vst2_f32(reinterpret_cast<float*>(odd), difference);
#else
    for (rstd::size_t index = 0; index < 2; ++index) {
        const auto even_value = even[index];
        const auto product    = Multiply(odd[index], weights[index]);
        even[index]           = { even_value.real + product.real, even_value.imag + product.imag };
        odd[index]            = { even_value.real - product.real, even_value.imag - product.imag };
    }
#endif
}

void Fill(Vec<Complex32>& values, rstd::size_t size) {
    values.clear();
    values.reserve(usize(size));
    for (rstd::size_t index = 0; index < size; ++index) values.push(Complex32 {});
}

void Prepare(Vec<Complex32>& values, rstd::size_t size) {
    if (values.len() != usize(size)) Fill(values, size);
}

} // namespace

DftPlan32::DftPlan32(rstd::size_t size): size_(size) {
    if (size_ < 2) rstd::panic { "DFT size must be at least two" };
    uses_chirp_       = (size_ & (size_ - 1)) != 0;
    convolution_size_ = uses_chirp_ ? NextPowerOfTwo(size_ * 2 - 1) : size_;

    rstd::size_t stage_count         = 0;
    rstd::size_t stage_twiddle_count = 0;
    for (rstd::size_t length = 2; length <= convolution_size_; length <<= 1) {
        ++stage_count;
        stage_twiddle_count += length / 2;
    }
    stage_offsets_.reserve(usize(stage_count));
    twiddles_.reserve(usize(stage_twiddle_count));

    constexpr double pi = f64::consts::PI.to_primitive();
    for (rstd::size_t length = 2; length <= convolution_size_; length <<= 1) {
        stage_offsets_.push(twiddles_.len());
        for (rstd::size_t index = 0; index < length / 2; ++index) {
            const double angle =
                -2.0 * pi * static_cast<double>(index) / static_cast<double>(length);
            twiddles_.push({
                .real = static_cast<float>(f64(angle).cos().to_primitive()),
                .imag = static_cast<float>(f64(angle).sin().to_primitive()),
            });
        }
    }

    if (! uses_chirp_) return;

    chirp_.reserve(usize(size_));
    conjugate_chirp_.reserve(usize(size_));
    for (rstd::size_t index = 0; index < size_; ++index) {
        const double position = static_cast<double>(index);
        const double angle    = pi * position * position / static_cast<double>(size_);
        const float  real     = static_cast<float>(f64(angle).cos().to_primitive());
        const float  imag     = static_cast<float>(f64(angle).sin().to_primitive());
        chirp_.push({ .real = real, .imag = imag });
        conjugate_chirp_.push({ .real = real, .imag = -imag });
    }

    Vec<Complex32> kernel;
    Fill(kernel, convolution_size_);
    Fill(kernel_spectrum_, convolution_size_);
    const float normalization = 1.0f / static_cast<float>(convolution_size_);
    kernel[usize()]           = { chirp_[usize()].real * normalization,
                                  chirp_[usize()].imag * normalization };
    for (rstd::size_t index = 1; index < size_; ++index) {
        const Complex32 value {
            .real = chirp_[usize(index)].real * normalization,
            .imag = chirp_[usize(index)].imag * normalization,
        };
        kernel[usize(index)]                     = value;
        kernel[usize(convolution_size_ - index)] = value;
    }
    power_forward(kernel.as_slice(), kernel_spectrum_.deref_mut());
}

void DftPlan32::power_forward(rstd::slice<Complex32>     input,
                              rstd::mut_ref<Complex32[]> output) const {
    if (input.len() != usize(convolution_size_) || output.len() != usize(convolution_size_))
        rstd::panic { "power FFT buffer size does not match its plan" };

    const bool   in_place = input.as_raw_ptr() == output.as_raw_ptr();
    rstd::size_t reversed = 0;
    for (rstd::size_t index = 0; index < convolution_size_; ++index) {
        if (in_place) {
            if (index < reversed) {
                const auto value        = output[usize(index)];
                output[usize(index)]    = output[usize(reversed)];
                output[usize(reversed)] = value;
            }
        } else {
            output[usize(reversed)] = input[usize(index)];
        }
        rstd::size_t bit = convolution_size_ >> 1;
        while ((reversed & bit) != 0) {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
    }

    rstd::size_t stage = 0;
    for (rstd::size_t length = 2; length <= convolution_size_; length <<= 1, ++stage) {
        const auto half    = length / 2;
        const auto weights = twiddles_.data() + stage_offsets_[usize(stage)].to_primitive();
        for (rstd::size_t base = 0; base < convolution_size_; base += length) {
            rstd::size_t offset = 0;
            for (; offset + 1 < half; offset += 2) {
                ButterflyPair(output.as_raw_ptr() + base + offset,
                              output.as_raw_ptr() + base + half + offset,
                              weights + offset);
            }
            for (; offset < half; ++offset) {
                const auto even    = output[usize(base + offset)];
                const auto product = Multiply(output[usize(base + half + offset)], weights[offset]);
                output[usize(base + offset)] = {
                    even.real + product.real,
                    even.imag + product.imag,
                };
                output[usize(base + half + offset)] = {
                    even.real - product.real,
                    even.imag - product.imag,
                };
            }
        }
    }
}

void DftPlan32::forward(rstd::slice<Complex32> input, rstd::mut_ref<Complex32[]> output,
                        DftWorkspace32& workspace) const {
    if (input.len() != usize(size_) || output.len() != usize(size_))
        rstd::panic { "DFT buffer size does not match its plan" };

    if (! uses_chirp_) {
        power_forward(input, output);
        return;
    }

    Prepare(workspace.first_, convolution_size_);
    Prepare(workspace.second_, convolution_size_);
    auto* first  = workspace.first_.data();
    auto* second = workspace.second_.data();

    rstd::size_t index = 0;
    for (; index + 1 < size_; index += 2)
        MultiplyPair(input.as_raw_ptr() + index, conjugate_chirp_.data() + index, first + index);
    if (index < size_) first[index] = Multiply(input[usize(index)], conjugate_chirp_[usize(index)]);
    for (index = size_; index < convolution_size_; ++index) first[index] = {};

    power_forward(workspace.first_.as_slice(), workspace.second_.deref_mut());

    for (index = 0; index + 1 < convolution_size_; index += 2) {
        Complex32 product[2];
        MultiplyPair(second + index, kernel_spectrum_.data() + index, product);
        first[index]     = { .real = product[0].imag, .imag = product[0].real };
        first[index + 1] = { .real = product[1].imag, .imag = product[1].real };
    }
    if (index < convolution_size_) {
        const auto product = Multiply(second[index], kernel_spectrum_[usize(index)]);
        first[index]       = { .real = product.imag, .imag = product.real };
    }

    power_forward(workspace.first_.as_slice(), workspace.second_.deref_mut());

    index = 0;
    for (; index + 1 < size_; index += 2) {
        Complex32 product[2];
        MultiplyPair(second + index, chirp_.data() + index, product);
        output[usize(index)]     = { .real = product[0].imag, .imag = product[0].real };
        output[usize(index + 1)] = { .real = product[1].imag, .imag = product[1].real };
    }
    if (index < size_) {
        const auto product   = Multiply(second[index], chirp_[usize(index)]);
        output[usize(index)] = { .real = product.imag, .imag = product.real };
    }
}

} // namespace owe::fft
