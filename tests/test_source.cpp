#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/source.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    std::vector<std::uint8_t> read_file(const std::string& path)
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        assert(file != nullptr && "could not open fixture");
        std::vector<std::uint8_t> bytes;
        std::uint8_t buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            bytes.insert(bytes.end(), buffer, buffer + read);
        }
        std::fclose(file);
        return bytes;
    }

    const char* fixture = "testsrc_64x48.png";

    void test_a_path_and_its_bytes_hash_the_same()
    {
        const std::string path = data_path(fixture);
        const std::vector<std::uint8_t> bytes = read_file(path);

        const Source from_path = Source::from_path(path);
        const Source from_bytes = Source::from_bytes(bytes);

        const std::string hash = from_path.sha256();
        assert(hash.starts_with("sha256:"));
        assert(hash.size() == std::string_view("sha256:").size() + 64);
        assert(hash == from_bytes.sha256());
    }

    void test_different_bytes_hash_differently()
    {
        std::vector<std::uint8_t> bytes = read_file(data_path(fixture));
        const Source original = Source::from_bytes(bytes);

        bytes.back() ^= 0xFF;
        const Source flipped = Source::from_bytes(bytes);

        assert(original.sha256() != flipped.sha256());
    }

    void test_from_memory_hashes_like_from_bytes()
    {
        const std::vector<std::uint8_t> bytes = read_file(data_path(fixture));
        const Source owned = Source::from_bytes(bytes);
        const Source borrowed = Source::from_memory(bytes);

        assert(owned.sha256() == borrowed.sha256());
    }

    void test_the_hash_is_cached_and_stable_across_threads()
    {
        const Source source = Source::from_bytes(read_file(data_path(fixture)));
        const std::string first = source.sha256();

        std::vector<std::string> results(8);
        std::vector<std::thread> threads;
        for (std::string& result : results)
        {
            threads.emplace_back([&] { result = source.sha256(); });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        for (const std::string& result : results)
        {
            assert(result == first);
        }
    }

    void test_decode_image_records_the_hash_and_not_the_path()
    {
        const std::string path = data_path(fixture);
        const Source source = Source::from_path(path);

        const DecodedImage decoded = decode_image(source);
        assert(decoded.evidence().source_sha256 == source.sha256());
        assert(decoded.evidence().source_sha256.starts_with("sha256:"));

        const json::Value document = decoded.processing_record().to_json();
        assert(document.dump().find(path) == std::string::npos);

        // Replaying the decode needs every option it ran with.
        const json::Value& configuration = document.at("configurations").at(0);
        assert(configuration.size() == 5);
        for (const char* option : {"pixel_format", "color", "assumed_color", "orientation", "strict"})
        {
            assert(configuration.contains(option));
        }
    }
}

int main()
{
    test_a_path_and_its_bytes_hash_the_same();
    test_different_bytes_hash_differently();
    test_from_memory_hashes_like_from_bytes();
    test_the_hash_is_cached_and_stable_across_threads();
    test_decode_image_records_the_hash_and_not_the_path();
    return 0;
}
