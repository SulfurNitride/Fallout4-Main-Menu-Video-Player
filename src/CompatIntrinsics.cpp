#include <cstddef>

// clang-cl occasionally leaves MinHook's MSVC __movsb intrinsic as an external
// when cross-compiling. MSVC normally lowers it inline, so provide the same
// semantics for the Linux-hosted Windows build.
extern "C" __declspec(noinline) void __movsb(
    unsigned char* destination,
    const unsigned char* source,
    std::size_t count)
{
    volatile unsigned char* output = destination;
    const volatile unsigned char* input = source;
    while (count-- != 0) {
        *output++ = *input++;
    }
}
