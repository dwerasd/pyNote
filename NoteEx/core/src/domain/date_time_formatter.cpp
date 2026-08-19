#include "pynote/core/domain/date_time_formatter.h"

#include <array>
#include <cstdlib>

namespace
{
	using pynote::core::domain::S_DATE_TIME_VIEW;

	constexpr std::array<std::string_view, 12> ShortMonths{
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	constexpr std::array<std::string_view, 12> LongMonths{
		"January", "February", "March", "April", "May", "June",
		"July", "August", "September", "October", "November", "December"
	};
	constexpr std::array<std::string_view, 7> ShortDays{
		"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
	};
	constexpr std::array<std::string_view, 7> LongDays{
		"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
	};

	bool is_leap(int year) noexcept
	{
		return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	}

	int days_in_month(int year, int month) noexcept
	{
		constexpr std::array<int, 12> days{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		if (month < 1 || month > 12) { return 0; }
		return month == 2 && is_leap(year) ? 29 : days[static_cast<std::size_t>(month - 1)];
	}

	bool valid(const S_DATE_TIME_VIEW& view) noexcept
	{
		return view.bValid && view.nYear != 0 && view.nMonth >= 1 && view.nMonth <= 12 &&
			view.nDay >= 1 && view.nDay <= days_in_month(view.nYear, view.nMonth) &&
			view.nHour >= 0 && view.nHour <= 23 && view.nMinute >= 0 && view.nMinute <= 59 &&
			view.nSecond >= 0 && view.nSecond <= 59 &&
			view.nMillisecond >= 0 && view.nMillisecond <= 999;
	}

	bool valid_utf8(std::string_view text) noexcept
	{
		for (std::size_t i = 0; i < text.size();) {
			const auto first = static_cast<unsigned char>(text[i]);
			std::size_t length = 0;
			std::uint32_t value = 0;
			if (first <= 0x7f) { ++i; continue; }
			if (first >= 0xc2 && first <= 0xdf) { length = 2; value = first & 0x1f; }
			else if (first >= 0xe0 && first <= 0xef) { length = 3; value = first & 0x0f; }
			else if (first >= 0xf0 && first <= 0xf4) { length = 4; value = first & 0x07; }
			else { return false; }
			if (i + length > text.size()) { return false; }
			for (std::size_t j = 1; j < length; ++j) {
				const auto next = static_cast<unsigned char>(text[i + j]);
				if ((next & 0xc0) != 0x80) { return false; }
				value = (value << 6) | (next & 0x3f);
			}
			if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
				value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) { return false; }
			i += length;
		}
		return true;
	}

	std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept
	{
		const auto quotient = value / divisor;
		const auto remainder = value % divisor;
		return remainder < 0 ? quotient - 1 : quotient;
	}

	int weekday(const S_DATE_TIME_VIEW& view) noexcept
	{
		std::int64_t year = view.nYear;
		if (year < 0) { ++year; }
		year -= view.nMonth <= 2;
		const auto era = floor_div(year, 400);
		const auto yearOfEra = year - era * 400;
		const auto adjustedMonth = view.nMonth + (view.nMonth > 2 ? -3 : 9);
		const auto dayOfYear = (153 * adjustedMonth + 2) / 5 + view.nDay - 1;
		const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
		const auto daysSinceEpoch = era * 146097 + dayOfEra - 719468;
		const auto mondayIndex = (daysSinceEpoch % 7 + 10) % 7;
		return static_cast<int>(mondayIndex);
	}

	void append_number(std::string& output, int value, int width)
	{
		std::string digits = std::to_string(std::abs(value));
		if (static_cast<int>(digits.size()) < width) {
			output.append(static_cast<std::size_t>(width - digits.size()), '0');
		}
		output += digits;
	}

	void append_year(std::string& output, int year)
	{
		if (year < 0) { output.push_back('-'); }
		append_number(output, year, 4);
	}

	bool starts_with(std::string_view text, std::size_t position, std::string_view token) noexcept
	{
		return position + token.size() <= text.size() && text.substr(position, token.size()) == token;
	}

	std::size_t skip_quoted(std::string_view format, std::size_t position) noexcept
	{
		if (position + 1 < format.size() && format[position + 1] == '\'') { return position + 2; }
		++position;
		while (position < format.size()) {
			if (format[position] != '\'') { ++position; continue; }
			if (position + 1 < format.size() && format[position + 1] == '\'') { position += 2; continue; }
			return position + 1;
		}
		return position;
	}

	bool has_am_pm(std::string_view format) noexcept
	{
		for (std::size_t i = 0; i < format.size();) {
			if (format[i] == '\'') { i = skip_quoted(format, i); continue; }
			if (format[i] == 'A' || format[i] == 'a') { return true; }
			++i;
		}
		return false;
	}

	void append_quoted(std::string& output, std::string_view format, std::size_t& position)
	{
		if (position + 1 < format.size() && format[position + 1] == '\'') {
			output.push_back('\''); position += 2; return;
		}
		++position;
		while (position < format.size()) {
			if (format[position] != '\'') { output.push_back(format[position++]); continue; }
			if (position + 1 < format.size() && format[position + 1] == '\'') {
				output.push_back('\''); position += 2; continue;
			}
			++position; return;
		}
	}

	void append_offset(std::string& output, std::int32_t seconds, bool colon)
	{
		const std::int64_t signedSeconds = seconds;
		const auto absolute = signedSeconds < 0 ? -signedSeconds : signedSeconds;
		output.push_back(signedSeconds < 0 ? '-' : '+');
		append_number(output, static_cast<int>(absolute / 3600), 2);
		if (colon) { output.push_back(':'); }
		append_number(output, static_cast<int>((absolute % 3600) / 60), 2);
	}

	void append_milliseconds(std::string& output, int millisecond, bool trim)
	{
		std::string value;
		append_number(value, millisecond, 3);
		if (trim) {
			while (value.size() > 1 && value.back() == '0') { value.pop_back(); }
		}
		output += value;
	}
}

namespace pynote::core::domain
{
	std::string FormatDateTime(const S_DATE_TIME_VIEW& _View, std::string_view _sFormat)
	{
		if (!valid(_View) || _sFormat.empty() || !valid_utf8(_sFormat)) { return {}; }
		const bool twelveHour = has_am_pm(_sFormat);
		std::string output;
		output.reserve(_sFormat.size() + 32);
		for (std::size_t i = 0; i < _sFormat.size();) {
			if (_sFormat[i] == '\'') { append_quoted(output, _sFormat, i); continue; }
			if (_sFormat[i] == 'd') {
				if (starts_with(_sFormat, i, "dddd")) { output += LongDays[weekday(_View)]; i += 4; }
				else if (starts_with(_sFormat, i, "ddd")) { output += ShortDays[weekday(_View)]; i += 3; }
				else if (starts_with(_sFormat, i, "dd")) { append_number(output, _View.nDay, 2); i += 2; }
				else { append_number(output, _View.nDay, 1); ++i; }
				continue;
			}
			if (_sFormat[i] == 'M') {
				if (starts_with(_sFormat, i, "MMMM")) { output += LongMonths[_View.nMonth - 1]; i += 4; }
				else if (starts_with(_sFormat, i, "MMM")) { output += ShortMonths[_View.nMonth - 1]; i += 3; }
				else if (starts_with(_sFormat, i, "MM")) { append_number(output, _View.nMonth, 2); i += 2; }
				else { append_number(output, _View.nMonth, 1); ++i; }
				continue;
			}
			if (starts_with(_sFormat, i, "yyyy")) { append_year(output, _View.nYear); i += 4; continue; }
			if (starts_with(_sFormat, i, "yy")) { append_number(output, std::abs(_View.nYear % 100), 2); i += 2; continue; }
			if (_sFormat[i] == 'h' || _sFormat[i] == 'H') {
				const char token = _sFormat[i]; const bool doubled = i + 1 < _sFormat.size() && _sFormat[i + 1] == token;
				int hour = _View.nHour;
				if (token == 'h' && twelveHour) { hour %= 12; if (hour == 0) { hour = 12; } }
				append_number(output, hour, doubled ? 2 : 1); i += doubled ? 2 : 1; continue;
			}
			if (_sFormat[i] == 'm' || _sFormat[i] == 's') {
				const char token = _sFormat[i]; const bool doubled = i + 1 < _sFormat.size() && _sFormat[i + 1] == token;
				append_number(output, token == 'm' ? _View.nMinute : _View.nSecond, doubled ? 2 : 1);
				i += doubled ? 2 : 1; continue;
			}
			if (_sFormat[i] == 'z') {
				if (starts_with(_sFormat, i, "zzz")) { append_milliseconds(output, _View.nMillisecond, false); i += 3; }
				else if (starts_with(_sFormat, i, "zz")) { append_milliseconds(output, _View.nMillisecond, true); i += 2; }
				else { append_milliseconds(output, _View.nMillisecond, true); ++i; }
				continue;
			}
			if (_sFormat[i] == 'A' || _sFormat[i] == 'a') {
				const bool doubled = i + 1 < _sFormat.size() && (_sFormat[i + 1] == 'P' || _sFormat[i + 1] == 'p');
				const bool upper = _sFormat[i] == 'A' || (doubled && _sFormat[i + 1] == 'P');
				output += _View.nHour < 12 ? (upper ? "AM" : "am") : (upper ? "PM" : "pm");
				i += doubled ? 2 : 1; continue;
			}
			if (_sFormat[i] == 't') {
				if (starts_with(_sFormat, i, "tttt")) { output += _View.sTimeZoneLongName; i += 4; }
				else if (starts_with(_sFormat, i, "ttt")) { append_offset(output, _View.nUtcOffsetSeconds, true); i += 3; }
				else if (starts_with(_sFormat, i, "tt")) { append_offset(output, _View.nUtcOffsetSeconds, false); i += 2; }
				else { output += _View.sTimeZoneAbbreviation; ++i; }
				continue;
			}
			output.push_back(_sFormat[i++]);
		}
		return output;
	}
}
