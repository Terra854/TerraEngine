#include "SourceReviewAudit.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace SourceReviewAudit
{
	namespace
	{
		// Match known boilerplate only inside comments. Concrete explanations of
		// a guard's invariant remain useful and must not be rejected generically.
		constexpr std::array<std::string_view, 4> patterns = {
			"is an implementation entry point;", "groups related state and operations",
			"Iterate over the current data snapshot",
			"Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data." };
		bool Ident(unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c >= 128; }
		bool Space(unsigned char c) { return std::isspace(c) != 0; }
		// Comment patches have no authority to reformat executable text. Keep its
		// exact spelling and spacing. Mask each comment byte in place, retaining tabs,
		// so equivalent padded comment deletion can keep executable byte columns.
		// Blank lines/trailing whitespace disappear only from this auxiliary view.
		std::string NonCommentSpelling(std::string_view source, const Result& result)
		{
			std::vector<bool> comment(source.size(), false);
			for (const auto& span : result.comments)
				for (auto i = span.begin; i < span.end; ++i) comment[i] = true;
			std::string filtered, line;
			auto flush = [&]
			{
				while (!line.empty() && Space(static_cast<unsigned char>(line.back()))) line.pop_back();
				if (!line.empty()) { filtered += line; filtered += '\n'; }
				line.clear();
			};
			for (std::size_t i = 0; i < source.size(); ++i)
			{
				if (source[i] == '\n') flush();
				else if (comment[i]) line += source[i] == '\t' ? '\t' : ' ';
				else line += source[i];
			}
			flush(); return filtered;
		}

		// Equal byte columns do not justify replacing tabs or UTF-8 prefixes with
		// spaces. Preserve their exact positions/bytes rather than guessing a
		// compiler's display-column or Unicode character-count convention.
		bool SameColumnPrefix(std::string_view before, const Token& left,
			std::string_view after, const Token& right)
		{
			if (left.column == 0) return true; // synthetic directive newline
			const auto leftStart = left.begin - (left.column - 1);
			const auto rightStart = right.begin - (right.column - 1);
			for (std::size_t offset = 0; offset + 1 < left.column; ++offset)
			{
				const auto a = static_cast<unsigned char>(before[leftStart + offset]);
				const auto b = static_cast<unsigned char>(after[rightStart + offset]);
				if ((a == '\t' || b == '\t' || a >= 128 || b >= 128) && a != b) return false;
			}
			return true;
		}

		// Translation phase two removes escaped newlines before recognizing C++
		// comments. origin keeps every output span tied to the untouched bytes.
		struct Lexer
		{
			std::string_view source;
			Language language;
			std::string text;
			std::vector<std::size_t> origin, lines;
			Result result;
			bool directive = false, lineStart = true;
			Lexer(std::string_view input, Language kind) : source(input), language(kind)
			{
				lines.push_back(0);
				for (std::size_t i = 0; i < input.size(); ++i)
				{
					// Bare CR is outside this lexer's LF/CRLF physical-line model.
					// Reject it in both languages instead of guessing how a compiler
					// translates an unsupported source-file line ending.
					// C# additionally recognizes NEL/LS/PS. Do not assume those C#
					// Unicode rules apply to C++; LF and CRLF remain supported.
					if ((input[i] == '\r' && (i + 1 == input.size() || input[i + 1] != '\n')) ||
						(kind == Language::CSharp && (input.substr(i, 2) == "\xc2\x85" ||
						 input.substr(i, 3) == "\xe2\x80\xa8" || input.substr(i, 3) == "\xe2\x80\xa9")))
						result.errors.push_back("line " + std::to_string(lines.size()) +
							": unsupported physical line terminator");
					if (input[i] == '\n') lines.push_back(i + 1);
					if (kind == Language::Cpp && input[i] == '\\' && i + 1 < input.size())
					{
						if (input[i + 1] == '\n') { ++i; lines.push_back(i + 1); continue; }
						if (input[i + 1] == '\r' && i + 2 < input.size() && input[i + 2] == '\n')
						{ i += 2; lines.push_back(i + 1); continue; }
					}
					origin.push_back(i); text.push_back(input[i]);
				}
				origin.push_back(input.size());
				result.codeLines.resize(lines.size(), false);
			}
			std::size_t Line(std::size_t byte) const
			{ return static_cast<std::size_t>(std::upper_bound(lines.begin(), lines.end(), byte) - lines.begin()); }
			bool At(std::size_t i, std::string_view word) const { return text.compare(i, word.size(), word) == 0; }
			void Error(std::size_t i, std::string_view message)
			{ result.errors.push_back("line " + std::to_string(Line(origin[std::min(i, text.size())])) + ": " + std::string(message)); }
			Span MakeSpan(std::size_t a, std::size_t b) const
			{
				Span span; span.begin = origin[a]; span.end = b == a ? origin[a] : origin[b - 1] + 1;
				span.firstLine = Line(span.begin); span.lastLine = Line(span.end > span.begin ? span.end - 1 : span.begin);
				return span;
			}
			void TokenAt(std::size_t a, std::size_t b)
			{
				const auto span = MakeSpan(a, b);
				result.tokens.push_back({text.substr(a, b - a), span.begin, span.end, span.firstLine,
					span.begin - lines[span.firstLine - 1] + 1});
				for (std::size_t p = a; p < b; ++p)
					if (!Space(static_cast<unsigned char>(text[p]))) result.codeLines[Line(origin[p]) - 1] = true;
				const auto& token = result.tokens.back().text;
				if (token == "__LINE__" || token == "__FILE__" || token == "source_location" ||
					token == "CallerLineNumber" || token == "CallerFilePath" || token == "line")
					result.sourceSensitive = true;
			}
			void Comment(std::size_t a, std::size_t b)
			{
				result.comments.push_back(MakeSpan(a, b));
				for (const auto pattern : patterns)
				{
					auto pos = text.find(pattern, a);
					while (pos < b && pos != std::string::npos)
					{
						if (pattern.size() <= b - pos) result.occurrences.push_back({origin[pos], Line(origin[pos]), std::string(pattern)});
						pos = text.find(pattern, pos + 1);
					}
				}
			}
			// Interpolation islands are traversed to find their actual delimiter.
			// The outer literal remains one exact token: edits inside an island are
			// conservatively rejected by Equivalent instead of silently normalized.
			std::size_t Island(std::size_t i)
			{
				std::size_t depth = 1;
				while (i < text.size())
				{
					const auto literal = Literal(i);
					if (literal != i) { i = literal; continue; }
					if (At(i, "//") || At(i, "/*")) { i = SkipComment(i); continue; }
					if (text[i] == '{') ++depth;
					if (text[i] == '}' && --depth == 0) return i + 1;
					++i;
				}
				Error(i, "unterminated interpolation expression"); return i;
			}
			std::size_t SkipComment(std::size_t i)
			{
				const auto start = i;
				if (At(i, "//")) { i = text.find('\n', i + 2); if (i == std::string::npos) i = text.size(); }
				else
				{
					i = text.find("*/", i + 2);
					if (i == std::string::npos) { Error(start, "unterminated block comment"); i = text.size(); }
					else i += 2;
				}
				const auto newline = text.find('\n', start);
				if (newline < i)
				{
					if (directive) result.tokens.push_back({"\n", origin[newline], origin[newline] + 1, Line(origin[newline])});
					directive = false; lineStart = true;
				}
				Comment(start, i); return i;
			}
			std::size_t Literal(std::size_t start)
			{
				auto i = start;
				bool verbatim = false, interpolated = false;
				if (language == Language::Cpp)
				{
					for (auto prefix : {std::string_view("u8"), std::string_view("u"), std::string_view("U"), std::string_view("L")})
						if (At(i, prefix) && i + prefix.size() < text.size() &&
							(text[i + prefix.size()] == '"' || text[i + prefix.size()] == '\'' || text[i + prefix.size()] == 'R'))
						{ i += prefix.size(); break; }
					if (At(i, "R\""))
					{
						const auto delimiterStart = i + 2, open = text.find('(', delimiterStart);
						if (open == std::string::npos || open - delimiterStart > 16 ||
							text.substr(delimiterStart, open - delimiterStart).find_first_of(" ()\\\t\r\n") != std::string::npos)
						{ Error(start, "invalid raw-string delimiter"); return text.size(); }
						const auto close = ")" + text.substr(delimiterStart, open - delimiterStart) + "\"";
						const auto end = text.find(close, open + 1);
						if (end == std::string::npos) { Error(start, "unterminated raw string"); return text.size(); }
						const auto spelling = source.substr(origin[start], origin[end + close.size() - 1] + 1 - origin[start]);
						if (spelling.find("\\\n") != std::string_view::npos || spelling.find("\\\r\n") != std::string_view::npos)
							Error(start, "unsupported escaped newline inside raw-string spelling");
						return end + close.size();
					}
				}
				else
				{
					if (At(i, "$$") || At(i, "$\"\"\"") || At(i, "@$\"\"\""))
					{ Error(start, "unsupported C# raw interpolation"); return text.size(); }
					if (At(i, "$@\"") || At(i, "@$\"")) { verbatim = interpolated = true; i += 2; }
					else if (At(i, "@\"")) { verbatim = true; ++i; }
					else if (At(i, "$\"")) { interpolated = true; ++i; }
					if (At(i, "\"\"\""))
					{
						auto quotes = std::size_t(3); while (i + quotes < text.size() && text[i + quotes] == '"') ++quotes;
						const auto end = text.find(std::string(quotes, '"'), i + quotes);
						if (end == std::string::npos) { Error(start, "unterminated C# raw string"); return text.size(); }
						return end + quotes;
					}
				}
				if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) return start;
				const char quote = text[i++];
				while (i < text.size())
				{
					if (!verbatim && text[i] == '\\') { i += std::min(std::size_t(2), text.size() - i); continue; }
					if (text[i] == quote)
					{
						if (verbatim && i + 1 < text.size() && text[i + 1] == quote) { i += 2; continue; }
						return i + 1;
					}
					if (interpolated && (At(i, "{{") || At(i, "}}"))) { i += 2; continue; }
					if (interpolated && text[i] == '{') { i = Island(i + 1); continue; }
					if (!verbatim && (text[i] == '\n' || text[i] == '\r')) { Error(start, "newline in quoted literal"); return text.size(); }
					++i;
				}
				Error(start, "unterminated quoted literal"); return i;
			}
			void Run()
			{
				std::vector<std::size_t> braceStack;
				auto i = std::size_t(0);
				if (At(0, "\xef\xbb\xbf")) i = 3;
				while (i < text.size())
				{
					if (Space(static_cast<unsigned char>(text[i])))
					{
						if (text[i] == '\n')
						{
							if (directive) result.tokens.push_back({"\n", origin[i], origin[i] + 1, Line(origin[i])});
							directive = false; lineStart = true;
						}
						++i; continue;
					}
					if (At(i, "//") || At(i, "/*")) { i = SkipComment(i); continue; }
					if (lineStart && text[i] == '#') directive = true;
					lineStart = false;
					// Header names are preprocessor tokens, so comment-looking bytes
					// inside <...> name a file rather than starting a comment.
					const auto tokenCount = result.tokens.size();
					const bool headerDirective = directive && tokenCount &&
						(result.tokens.back().text == "include" || result.tokens.back().text == "include_next" || result.tokens.back().text == "import");
					const bool headerProbe = tokenCount > 1 && result.tokens.back().text == "(" &&
						(result.tokens[tokenCount - 2].text == "__has_include" || result.tokens[tokenCount - 2].text == "__has_include_next");
					const bool headerUnit = tokenCount && result.tokens.back().text == "import";
					if (language == Language::Cpp && text[i] == '<' && (headerDirective || headerProbe || headerUnit))
					{
						const auto end = text.find('>', i + 1), newline = text.find('\n', i + 1);
						if (end == std::string::npos || newline < end) { Error(i, "unterminated header name"); break; }
						TokenAt(i, end + 1); i = end + 1; continue;
					}
					const auto literal = Literal(i);
					if (literal != i)
					{
						auto end = literal;
						// C++ user-defined suffixes are part of the literal token;
						// inserting whitespace before one can change compilation.
						if (language == Language::Cpp)
							while (end < text.size() && Ident(static_cast<unsigned char>(text[end]))) ++end;
						TokenAt(i, end);
						// Raw literals restore phase-two spelling. Retaining the
						// original bytes for every literal is conservative and also
						// prevents hidden payload edits around escaped newlines.
						auto& token = result.tokens.back();
						token.text = std::string(source.substr(token.begin, token.end - token.begin));
						i = end; continue;
					}
					const auto start = i;
					if (std::isdigit(static_cast<unsigned char>(text[i])) || (text[i] == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))))
					{
						++i;
						while (i < text.size() && (Ident(static_cast<unsigned char>(text[i])) || text[i] == '.' || text[i] == '\'' ||
							((text[i] == '+' || text[i] == '-') && (text[i - 1] == 'e' || text[i - 1] == 'E' || text[i - 1] == 'p' || text[i - 1] == 'P')))) ++i;
					}
					else if (Ident(static_cast<unsigned char>(text[i]))) { while (i < text.size() && Ident(static_cast<unsigned char>(text[i]))) ++i; }
					else
					{
						// Maximal munch matters: deleting a separating comment must not
						// transform two '+' tokens into the increment operator.
						std::size_t width = 1;
						for (auto op : {"%:%:", ">>=", "<<=", "<=>", "->*", "...", "::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "^=", "&=", "|=", ".*", "##", "<:", ":>", "<%", "%>", "%:", "=>", "??", "?.", "?["})
							if (At(i, op)) width = std::max(width, std::string_view(op).size());
						i += width;
					}
					TokenAt(start, i);
					const auto& token = result.tokens.back();
					if (token.text == "{" || token.text == "<%") braceStack.push_back(result.tokens.size() - 1);
					if (token.text == "}" || token.text == "%>")
					{
						if (braceStack.empty()) Error(start, "unmatched closing brace");
						else
						{
							const auto& open = result.tokens[braceStack.back()]; braceStack.pop_back();
							result.braces.push_back({open.begin, token.end, open.line, token.line, 0});
						}
					}
				}
				if (directive) result.tokens.push_back({"\n", source.size(), source.size(), Line(source.size())});
				if (!braceStack.empty()) Error(text.size(), "unbounded opening brace");
				for (auto& span : result.braces)
					for (auto line = span.firstLine; line <= span.lastLine; ++line) if (result.codeLines[line - 1]) ++span.codeLines;
				std::sort(result.braces.begin(), result.braces.end(), [](const Span& a, const Span& b) { return a.begin < b.begin; });
			}
		};
	}
	Result Scan(std::string_view source, Language language)
	{
		Lexer lexer(source, language); lexer.Run(); return std::move(lexer.result);
	}
	bool Equivalent(std::string_view before, std::string_view after, Language language, std::string& reason)
	{
		const auto a = Scan(before, language), b = Scan(after, language);
		if (!a.ok() || !b.ok()) { reason = "invalid or unsupported source"; return false; }
		// Headers may import __LINE__-sensitive macros without mentioning them in
		// this file. Preserve physical newlines, executable token lines and columns
		// universally rather than guessing indirect source-location dependencies.
		if (a.codeLines.size() != b.codeLines.size()) { reason = "physical newline count differs"; return false; }
		if (a.tokens.size() != b.tokens.size()) { reason = "executable token count differs"; return false; }
		for (std::size_t i = 0; i < a.tokens.size(); ++i)
		{
			if (a.tokens[i].text != b.tokens[i].text) { reason = "executable token differs at index " + std::to_string(i); return false; }
			if (a.tokens[i].line != b.tokens[i].line)
			{ reason = "physical token line differs at index " + std::to_string(i); return false; }
			if (a.tokens[i].column != b.tokens[i].column)
			{ reason = "physical token byte column differs at index " + std::to_string(i); return false; }
			if (!SameColumnPrefix(before, a.tokens[i], after, b.tokens[i]))
			{ reason = "tab or non-ASCII token prefix spelling differs at index " + std::to_string(i); return false; }
		}
		if (NonCommentSpelling(before, a) != NonCommentSpelling(after, b))
		{ reason = "non-comment executable spelling or spacing differs"; return false; }
		reason = "equivalent executable tokens"; return true;
	}
	const Span* MeasureSpan(const Result& result, std::size_t openingBraceOffset)
	{
		if (!result.ok()) return nullptr;
		for (const auto& span : result.braces) if (span.begin == openingBraceOffset) return &span;
		return nullptr;
	}
}
