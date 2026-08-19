#include "pynote/core/domain/diffing.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace pynote::core::domain
{
	namespace
	{
		struct Match { std::size_t a{}; std::size_t b{}; std::size_t size{}; };
		struct Opcode { E_DIFF_TAG tag{}; std::size_t a0{}, a1{}, b0{}, b1{}; };

		std::size_t scalar_length(unsigned char lead) noexcept
		{
			if (lead < 0x80) { return 1; }
			if ((lead & 0xE0) == 0xC0) { return 2; }
			if ((lead & 0xF0) == 0xE0) { return 3; }
			return 4;
		}

		std::vector<std::string> scalars(std::string_view text)
		{
			std::vector<std::string> result;
			for (std::size_t offset = 0; offset < text.size();) {
				const std::size_t length = scalar_length(static_cast<unsigned char>(text[offset]));
				result.emplace_back(text.substr(offset, length));
				offset += length;
			}
			return result;
		}

		bool line_break(std::string_view scalar) noexcept
		{
			if (scalar.size() == 1) {
				const unsigned char value = static_cast<unsigned char>(scalar[0]);
				return value == 0x0A || value == 0x0B || value == 0x0C || value == 0x0D
					|| (value >= 0x1C && value <= 0x1E);
			}
			return scalar == "\xC2\x85" || scalar == "\xE2\x80\xA8" || scalar == "\xE2\x80\xA9";
		}

		std::vector<std::string> split_lines(std::string_view text)
		{
			std::vector<std::string> result;
			std::size_t lineStart = 0;
			for (std::size_t offset = 0; offset < text.size();) {
				const std::size_t length = scalar_length(static_cast<unsigned char>(text[offset]));
				const std::string_view scalar = text.substr(offset, length);
				if (!line_break(scalar)) { offset += length; continue; }
				offset += length;
				if (scalar == "\r" && offset < text.size() && text[offset] == '\n') { ++offset; }
				result.emplace_back(text.substr(lineStart, offset - lineStart));
				lineStart = offset;
			}
			if (lineStart < text.size()) { result.emplace_back(text.substr(lineStart)); }
			return result;
		}

		template<typename Token>
		class SequenceMatcher
		{
		public:
			SequenceMatcher(const std::vector<Token>& before, const std::vector<Token>& after)
				: before_(before), after_(after)
			{
				for (std::size_t index = 0; index < after_.size(); ++index) { positions_[after_[index]].push_back(index); }
			}

			std::vector<Opcode> opcodes() const
			{
				std::vector<Match> matches;
				std::vector<std::tuple<std::size_t,std::size_t,std::size_t,std::size_t>> queue{
					{ 0, before_.size(), 0, after_.size() }
				};
				while (!queue.empty()) {
					auto [a0,a1,b0,b1] = queue.back(); queue.pop_back();
					const Match match = longest(a0,a1,b0,b1);
					if (match.size == 0) { continue; }
					matches.push_back(match);
					if (a0 < match.a && b0 < match.b) { queue.emplace_back(a0,match.a,b0,match.b); }
					if (match.a + match.size < a1 && match.b + match.size < b1) {
						queue.emplace_back(match.a + match.size,a1,match.b + match.size,b1);
					}
				}
				std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
					return std::tie(left.a,left.b,left.size) < std::tie(right.a,right.b,right.size);
				});
				std::vector<Match> merged;
				for (const Match& match : matches) {
					if (!merged.empty() && merged.back().a + merged.back().size == match.a
						&& merged.back().b + merged.back().size == match.b) { merged.back().size += match.size; }
					else { merged.push_back(match); }
				}
				merged.push_back({ before_.size(), after_.size(), 0 });
				std::vector<Opcode> result; std::size_t a = 0, b = 0;
				for (const Match& match : merged) {
					if (a < match.a && b < match.b) { result.push_back({E_DIFF_TAG::Replace,a,match.a,b,match.b}); }
					else if (a < match.a) { result.push_back({E_DIFF_TAG::Delete,a,match.a,b,match.b}); }
					else if (b < match.b) { result.push_back({E_DIFF_TAG::Insert,a,match.a,b,match.b}); }
					if (match.size != 0) { result.push_back({E_DIFF_TAG::Equal,match.a,match.a+match.size,match.b,match.b+match.size}); }
					a = match.a + match.size; b = match.b + match.size;
				}
				return result;
			}

		private:
			Match longest(std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1) const
			{
				Match best{a0,b0,0}; std::map<std::size_t,std::size_t> previous;
				for (std::size_t i = a0; i < a1; ++i) {
					std::map<std::size_t,std::size_t> current;
					const auto found = positions_.find(before_[i]);
					if (found == positions_.end()) { previous.clear(); continue; }
					for (const std::size_t j : found->second) {
						if (j < b0) { continue; }
						if (j >= b1) { break; }
						const auto predecessor = previous.find(j - (j != 0 ? 1 : 0));
						const std::size_t length = j != 0 && predecessor != previous.end() ? predecessor->second + 1 : 1;
						current[j] = length;
						if (length > best.size) { best = { i - length + 1, j - length + 1, length }; }
					}
					previous = std::move(current);
				}
				return best;
			}

			const std::vector<Token>& before_;
			const std::vector<Token>& after_;
			std::map<Token,std::vector<std::size_t>> positions_;
		};

		std::string join(const std::vector<std::string>& tokens, std::size_t first, std::size_t last)
		{
			std::string result;
			for (std::size_t index = first; index < last; ++index) { result += tokens[index]; }
			return result;
		}
	}

	std::string_view ToText(E_DIFF_TAG tag) noexcept
	{
		switch (tag) {
		case E_DIFF_TAG::Equal: return "equal";
		case E_DIFF_TAG::Insert: return "insert";
		case E_DIFF_TAG::Delete: return "delete";
		default: return "replace";
		}
	}

	std::vector<S_CHARACTER_DIFF> DiffCharacters(std::string_view before, std::string_view after)
	{
		const auto beforeTokens = scalars(before); const auto afterTokens = scalars(after);
		std::vector<S_CHARACTER_DIFF> result;
		for (const Opcode& opcode : SequenceMatcher(beforeTokens,afterTokens).opcodes()) {
			result.push_back({opcode.tag,join(beforeTokens,opcode.a0,opcode.a1),join(afterTokens,opcode.b0,opcode.b1)});
		}
		return result;
	}

	S_TEXT_DIFF DiffText(std::string_view before, std::string_view after)
	{
		S_TEXT_DIFF result{std::string(before),std::string(after),{}};
		const auto beforeLines = split_lines(before); const auto afterLines = split_lines(after);
		for (const Opcode& opcode : SequenceMatcher(beforeLines,afterLines).opcodes()) {
			if (opcode.tag == E_DIFF_TAG::Equal) {
				for (std::size_t offset=0; offset<opcode.a1-opcode.a0; ++offset) {
					result.Lines.push_back({E_DIFF_TAG::Equal,opcode.a0+offset+1,opcode.b0+offset+1,beforeLines[opcode.a0+offset],afterLines[opcode.b0+offset],{}});
				}
			}
			else if (opcode.tag == E_DIFF_TAG::Delete) {
				for (std::size_t index=opcode.a0; index<opcode.a1; ++index) { result.Lines.push_back({E_DIFF_TAG::Delete,index+1,std::nullopt,beforeLines[index],"",{}}); }
			}
			else if (opcode.tag == E_DIFF_TAG::Insert) {
				for (std::size_t index=opcode.b0; index<opcode.b1; ++index) { result.Lines.push_back({E_DIFF_TAG::Insert,std::nullopt,index+1,"",afterLines[index],{}}); }
			}
			else {
				const std::size_t paired = (std::min)(opcode.a1-opcode.a0,opcode.b1-opcode.b0);
				for (std::size_t offset=0; offset<paired; ++offset) {
					const auto& a=beforeLines[opcode.a0+offset]; const auto& b=afterLines[opcode.b0+offset];
					result.Lines.push_back({E_DIFF_TAG::Replace,opcode.a0+offset+1,opcode.b0+offset+1,a,b,DiffCharacters(a,b)});
				}
				for (std::size_t index=opcode.a0+paired; index<opcode.a1; ++index) { result.Lines.push_back({E_DIFF_TAG::Delete,index+1,std::nullopt,beforeLines[index],"",{}}); }
				for (std::size_t index=opcode.b0+paired; index<opcode.b1; ++index) { result.Lines.push_back({E_DIFF_TAG::Insert,std::nullopt,index+1,"",afterLines[index],{}}); }
			}
		}
		return result;
	}
}
