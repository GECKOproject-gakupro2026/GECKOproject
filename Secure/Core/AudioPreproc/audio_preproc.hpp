#ifndef AUDIO_PREPROC_HPP
#define AUDIO_PREPROC_HPP

#include <cstddef>
#include <cstdint>

namespace audio_preproc
{

constexpr std::size_t kMelBands = 64U;
constexpr std::size_t kColumns = 96U;
constexpr std::size_t kHopSamples = 160U;
constexpr std::size_t kWindowSamples = 400U;
constexpr std::size_t kPatchSamples =
    (kColumns - 1U) * kHopSamples + kWindowSamples;
constexpr std::size_t kFeatureCount = kMelBands * kColumns;

bool init();
bool run(const int16_t *pcm, std::size_t sampleCount, int8_t *features);

} // namespace audio_preproc

#endif /* AUDIO_PREPROC_HPP */
