#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pynote::core::domain
{
	class I_PARAGRAPH_SPLIT_POLICY
	{
	public:
		virtual ~I_PARAGRAPH_SPLIT_POLICY() = default;
		virtual std::vector<std::string> Split(std::string_view _sText) const = 0;
	};

	class C_BLANK_LINE_PARAGRAPH_POLICY final : public I_PARAGRAPH_SPLIT_POLICY
	{
	public:
		std::vector<std::string> Split(std::string_view _sText) const override;
	};

	class C_PARAGRAPH_PARSER
	{
	public:
		C_PARAGRAPH_PARSER();
		explicit C_PARAGRAPH_PARSER(std::shared_ptr<const I_PARAGRAPH_SPLIT_POLICY> _pPolicy);

		std::vector<std::string> Split(std::string_view _sText) const;
		bool IsZeroParagraphInput(std::string_view _sText) const;
		std::string Keep(std::string_view _sText) const;

	private:
		std::shared_ptr<const I_PARAGRAPH_SPLIT_POLICY> m_pPolicy;
	};
}
