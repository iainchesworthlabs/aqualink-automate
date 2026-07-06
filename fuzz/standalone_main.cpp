//
// Standalone replay driver for the fuzz harnesses.
//
// libFuzzer (clang -fsanitize=fuzzer) supplies its own main() and coverage-guided
// mutation engine.  When the fuzz targets are built with any other compiler (e.g.
// MSVC) there is no libFuzzer, so this file provides a plain main() that replays a
// fixed set of inputs — every file named on the command line, or every regular
// file inside each directory named — through LLVMFuzzerTestOneInput().
//
// This makes the harnesses runnable everywhere for corpus replay / crash
// reproduction / CI smoke testing without a coverage-guided fuzzing engine, and is
// the exact contract OSS-Fuzz's afl/standalone drivers follow.  It is compiled into
// the target ONLY when the libFuzzer engine is unavailable (see fuzz/CMakeLists.txt).
//

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

// Optional one-time init (each harness defines it to quieten logging etc.).
extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv);

namespace
{

	std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
	}

	int RunOne(const std::filesystem::path& path)
	{
		const std::vector<uint8_t> bytes = ReadFile(path);
		LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
		std::cout << "  ok: " << path.string() << " (" << bytes.size() << " bytes)\n";
		return 1;
	}

}
// unnamed namespace

int main(int argc, char** argv)
{
	LLVMFuzzerInitialize(&argc, &argv);

	if (argc < 2)
	{
		std::cerr << "usage: " << argv[0] << " <file-or-directory> [more ...]\n"
		          << "Replays each input through the fuzz target (no coverage-guided mutation).\n";
		return 2;
	}

	int count = 0;

	for (int i = 1; i < argc; ++i)
	{
		const std::filesystem::path arg(argv[i]);

		if (std::filesystem::is_directory(arg))
		{
			for (const auto& entry : std::filesystem::directory_iterator(arg))
			{
				if (entry.is_regular_file())
				{
					count += RunOne(entry.path());
				}
			}
		}
		else if (std::filesystem::is_regular_file(arg))
		{
			count += RunOne(arg);
		}
		else
		{
			std::cerr << "  skip (not a file/dir): " << arg.string() << '\n';
		}
	}

	std::cout << "Replayed " << count << " input(s) with no crash.\n";
	return 0;
}
