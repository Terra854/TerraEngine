#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// This validation-only lexer reports original UTF-8 byte offsets. It does not
// claim to resolve C++ types or discover semantic function names: reviewed stage
// manifests select an exact opening brace from the complete brace inventory.
namespace SourceReviewAudit
{
	enum class Language { Cpp, CSharp };
	struct Token
	{
		std::string text;
		// One-based original byte column; zero is reserved for a synthetic
		// preprocessor newline token, which has no executable column.
		std::size_t begin = 0, end = 0, line = 0, column = 0;
	};
	struct Span
	{
		std::size_t begin = 0, end = 0, firstLine = 0, lastLine = 0, codeLines = 0;
	};
	struct Occurrence
	{
		std::size_t offset = 0, line = 0;
		std::string pattern;
	};
	struct Result
	{
		std::vector<Token> tokens;
		std::vector<Span> comments, braces;
		std::vector<Occurrence> occurrences;
		std::vector<std::string> errors;
		std::vector<bool> codeLines;
		bool sourceSensitive = false;
		bool ok() const { return errors.empty(); }
	};
	// Scan never rewrites source. Invalid or unsupported lexical forms are errors.
	Result Scan(std::string_view source, Language language = Language::Cpp);
	// Tokens retain punctuation boundaries, literals and directive newlines.
	// Every file preserves physical token lines and original byte columns, including
	// tab/non-ASCII prefix spelling. Includes can import location-sensitive macros.
	// This conservatively preserves spelling, not a universal display-column model.
	bool Equivalent(std::string_view before, std::string_view after, Language language,
		std::string& reason);
	const Span* MeasureSpan(const Result& result, std::size_t openingBraceOffset);
}
