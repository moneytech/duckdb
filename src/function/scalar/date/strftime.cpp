#include "duckdb/function/scalar/date_functions.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/numeric_helper.hpp"

#include "duckdb/common/vector_operations/unary_executor.hpp"

#include "duckdb/execution/expression_executor.hpp"

#include "re2/re2.h"

namespace duckdb {

enum class StrTimeSpecifier : uint8_t {
	ABBREVIATED_WEEKDAY_NAME = 0,          // %a - Abbreviated weekday name. (Sun, Mon, ...)
	FULL_WEEKDAY_NAME = 1,                 // %A Full weekday name. (Sunday, Monday, ...)
	WEEKDAY_DECIMAL = 2,                   // %w - Weekday as a decimal number. (0, 1, ..., 6)
	DAY_OF_MONTH_PADDED = 3,               // %d - Day of the month as a zero-padded decimal. (01, 02, ..., 31)
	DAY_OF_MONTH = 4,                      // %-d - Day of the month as a decimal number. (1, 2, ..., 30)
	ABBREVIATED_MONTH_NAME = 5,            // %b - Abbreviated month name. (Jan, Feb, ..., Dec)
	FULL_MONTH_NAME = 6,                   // %B - Full month name. (January, February, ...)
	MONTH_DECIMAL_PADDED = 7,              // %m - Month as a zero-padded decimal number. (01, 02, ..., 12)
	MONTH_DECIMAL = 8,                     // %-m - Month as a decimal number. (1, 2, ..., 12)
	YEAR_WITHOUT_CENTURY_PADDED = 9,       // %y - Year without century as a zero-padded decimal number. (00, 01, ..., 99)
	YEAR_WITHOUT_CENTURY = 10,             // %-y - Year without century as a decimal number. (0, 1, ..., 99)
	YEAR_DECIMAL = 11,                     // %Y - Year with century as a decimal number. (2013, 2019 etc.)
	HOUR_24_PADDED = 12,                   // %H - Hour (24-hour clock) as a zero-padded decimal number. (00, 01, ..., 23)
	HOUR_24_DECIMAL = 13,                  // %-H - Hour (24-hour clock) as a decimal number. (0, 1, ..., 23)
	HOUR_12_PADDED = 14,                   // %I - Hour (12-hour clock) as a zero-padded decimal number. (01, 02, ..., 12)
	HOUR_12_DECIMAL = 15,                  // %-I - Hour (12-hour clock) as a decimal number. (1, 2, ... 12)
	AM_PM = 16,                            // %p - Locale’s AM or PM. (AM, PM)
	MINUTE_PADDED = 17,                    // %M - Minute as a zero-padded decimal number. (00, 01, ..., 59)
	MINUTE_DECIMAL = 18,                   // %-M - Minute as a decimal number. (0, 1, ..., 59)
	SECOND_PADDED = 19,                    // %S - Second as a zero-padded decimal number. (00, 01, ..., 59)
	SECOND_DECIMAL = 20,                   // %-S - Second as a decimal number. (0, 1, ..., 59)
	MICROSECOND_PADDED = 21,               // %f - Microsecond as a decimal number, zero-padded on the left. (000000 - 999999)
	UTC_OFFSET = 22,                       // %z - UTC offset in the form +HHMM or -HHMM. ( )
	TZ_NAME = 23,                          // %Z - Time zone name. ( )
	DAY_OF_YEAR_PADDED = 24,               // %j - Day of the year as a zero-padded decimal number. (001, 002, ..., 366)
	DAY_OF_YEAR_DECIMAL = 25,              // %-j - Day of the year as a decimal number. (1, 2, ..., 366)
	WEEK_NUMBER_PADDED_SUN_FIRST = 26,     // %U - Week number of the year (Sunday as the first day of the week). All days in a new year preceding the first Sunday are considered to be in week 0. (00, 01, ..., 53)
	WEEK_NUMBER_PADDED_MON_FIRST = 27,     // %W - Week number of the year (Monday as the first day of the week). All days in a new year preceding the first Monday are considered to be in week 0. (00, 01, ..., 53)
	LOCALE_APPROPRIATE_DATE_AND_TIME = 28, // %c - Locale’s appropriate date and time representation. (Mon Sep 30 07:06:05 2013)
	LOCALE_APPROPRIATE_DATE = 29,          // %x - Locale’s appropriate date representation. (09/30/13)
	LOCALE_APPROPRIATE_TIME = 30           // %X - Locale’s appropriate time representation. (07:06:05)
};

idx_t StrfTimepecifierSize(StrTimeSpecifier specifier) {
	switch(specifier) {
	case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
	case StrTimeSpecifier::ABBREVIATED_MONTH_NAME:
		return 3;
	case StrTimeSpecifier::WEEKDAY_DECIMAL:
		return 1;
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
	case StrTimeSpecifier::HOUR_24_PADDED:
	case StrTimeSpecifier::HOUR_12_PADDED:
	case StrTimeSpecifier::MINUTE_PADDED:
	case StrTimeSpecifier::SECOND_PADDED:
	case StrTimeSpecifier::AM_PM:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
		return 2;
	case StrTimeSpecifier::MICROSECOND_PADDED:
		return 6;
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		return 3;
	default:
		return 0;
	}
}

struct StrTimeFormat {
	//! The format specifiers
	vector<StrTimeSpecifier> specifiers;
	//! The literals that appear in between the format specifiers
	//! The following must hold: literals.size() = specifiers.size() + 1
	//! Format is literals[0], specifiers[0], literals[1], ..., specifiers[n - 1], literals[n]
	vector<string> literals;
	//! The constant size that appears in the format string
	idx_t constant_size;
	//! Whether or not the specifier is a numeric specifier (i.e. is parsed as a number)
	vector<bool> is_numeric;

	void AddLiteral(string literal) {
		constant_size += literal.size();
		literals.push_back(move(literal));
	}

	virtual void AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) {
		AddLiteral(move(preceding_literal));
		specifiers.push_back(specifier);
	}
};

struct StrfTimeFormat : public StrTimeFormat {
	//! The variable-length specifiers. To determine total string size, these need to be checked.
	vector<StrTimeSpecifier> var_length_specifiers;
	//! Whether or not the current specifier is a special "date" specifier (i.e. one that requires a date_t object to generate)
	vector<bool> is_date_specifier;

	void AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) override {
		is_date_specifier.push_back(IsDateSpecifier(specifier));
		idx_t specifier_size = StrfTimepecifierSize(specifier);
		if (specifier_size == 0) {
			// variable length specifier
			var_length_specifiers.push_back(specifier);
		} else {
			// constant size specifier
			constant_size += specifier_size;
		}
		StrTimeFormat::AddFormatSpecifier(move(preceding_literal), specifier);
	}

	static idx_t GetSpecifierLength(StrTimeSpecifier specifier, date_t date, time_t time) {
		switch(specifier) {
		case StrTimeSpecifier::FULL_WEEKDAY_NAME:
			return Date::DayNames[Date::ExtractISODayOfTheWeek(date) % 7].GetSize();
		case StrTimeSpecifier::FULL_MONTH_NAME:
			return Date::MonthNames[Date::ExtractMonth(date) - 1].GetSize();
		case StrTimeSpecifier::YEAR_DECIMAL: {
			auto year = Date::ExtractYear(date);
			return NumericHelper::SignedLength<int32_t, uint32_t>(year);
		}
		case StrTimeSpecifier::MONTH_DECIMAL: {
			idx_t len = 1;
			auto month = Date::ExtractMonth(date);
			len += month >= 10;
			return len;
		}
		case StrTimeSpecifier::UTC_OFFSET:
		case StrTimeSpecifier::TZ_NAME:
			// empty for now
			return 0;
		case StrTimeSpecifier::HOUR_24_DECIMAL:
		case StrTimeSpecifier::HOUR_12_DECIMAL:
		case StrTimeSpecifier::MINUTE_DECIMAL:
		case StrTimeSpecifier::SECOND_DECIMAL: {
			// time specifiers
			idx_t len = 1;
			int32_t hour, min, sec, msec;
			Time::Convert(time, hour, min, sec, msec);
			switch(specifier) {
			case StrTimeSpecifier::HOUR_24_DECIMAL:
				len += hour >= 10;
				break;
			case StrTimeSpecifier::HOUR_12_DECIMAL:
				hour = hour % 12;
				if (hour == 0) {
					hour = 12;
				}
				len += hour >= 10;
				break;
			case StrTimeSpecifier::MINUTE_DECIMAL:
				len += min >= 10;
				break;
			case StrTimeSpecifier::SECOND_DECIMAL:
				len += sec >= 10;
				break;
			default:
				break;
			}
			return len;
		}
		case StrTimeSpecifier::DAY_OF_MONTH:
			return NumericHelper::UnsignedLength<uint32_t>(Date::ExtractDay(date));
		case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
			return NumericHelper::UnsignedLength<uint32_t>(Date::ExtractDayOfTheYear(date));
		case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
			return NumericHelper::UnsignedLength<uint32_t>(Date::ExtractYear(date) % 100);
		default:
			throw NotImplementedException("Unimplemented specifier for GetSpecifierLength");
		}
	}

	//! Returns the total length of the date formatted by this format specifier
	idx_t GetLength(date_t date, time_t time) {
		idx_t size = constant_size;
		if (var_length_specifiers.size() > 0) {
			for(auto &specifier : var_length_specifiers) {
				size += GetSpecifierLength(specifier, date, time);
			}
		}
		return size;
	}

	char* WriteString(char *target, string_t &str) {
		idx_t size = str.GetSize();
		memcpy(target, str.GetData(), str.GetSize());
		return target + size;
	}

	// write a value in the range of 0..99 unpadded (e.g. "1", "2", ... "98", "99")
	char *Write2(char *target, uint8_t value) {
		if (value >= 10) {
			return WritePadded2(target, value);
		} else {
			*target = '0' + value;
			return target + 1;
		}
	}

	// write a value in the range of 0..99 padded to 2 digits
	char* WritePadded2(char *target, int32_t value) {
		auto index = static_cast<unsigned>(value * 2);
		*target++ = duckdb_fmt::internal::data::digits[index];
		*target++ = duckdb_fmt::internal::data::digits[index + 1];
		return target;
	}

	// write a value in the range of 0..999 padded
	char *WritePadded3(char *target, uint32_t value) {
		if (value >= 100) {
			WritePadded2(target + 1, value % 100);
			*target = '0' + value / 100;
			return target + 3;
		} else {
			*target = '0';
			target++;
			return WritePadded2(target, value);
		}
	}

	// write a value in the range of 0..999999 padded to 6 digits
	char* WritePadded(char *target, int32_t value, int32_t padding) {
		assert(padding % 2 == 0);
		for(int i = 0; i < padding / 2; i++) {
			int decimals = value % 100;
			WritePadded2(target + padding - 2 * (i + 1), decimals);
			value /= 100;
		}
		return target + padding;
	}

	bool IsDateSpecifier(StrTimeSpecifier specifier) {
		switch(specifier) {
		case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
		case StrTimeSpecifier::FULL_WEEKDAY_NAME:
		case StrTimeSpecifier::WEEKDAY_DECIMAL:
		case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
		case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
			return true;
		default:
			return false;
		}
	}

	char* WriteDateSpecifier(StrTimeSpecifier specifier, date_t date, char *target) {
		switch(specifier) {
		case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME: {
			date_t dow = Date::ExtractISODayOfTheWeek(date);
			target = WriteString(target, Date::DayNamesAbbreviated[dow % 7]);
			break;
		}
		case StrTimeSpecifier::FULL_WEEKDAY_NAME: {
			date_t dow = Date::ExtractISODayOfTheWeek(date);
			target = WriteString(target, Date::DayNames[dow % 7]);
			break;
		}
		case StrTimeSpecifier::WEEKDAY_DECIMAL: {
			date_t dow = Date::ExtractISODayOfTheWeek(date);
			*target = '0' + (dow % 7);
			target++;
			break;
		}
		case StrTimeSpecifier::DAY_OF_YEAR_PADDED: {
			int32_t doy = Date::ExtractDayOfTheYear(date);
			target = WritePadded3(target, doy);
			break;
		}
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
			target = WritePadded2(target, Date::ExtractWeekNumberRegular(date, true));
			break;
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
			target = WritePadded2(target, Date::ExtractWeekNumberRegular(date, false));
			break;
		case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL: {
			uint32_t doy = Date::ExtractDayOfTheYear(date);
			target += NumericHelper::UnsignedLength<uint32_t>(doy);
			NumericHelper::FormatUnsigned(doy, target);
			break;
		}
		default:
			throw NotImplementedException("Unimplemented date specifier for strftime");
		}
		return target;
	}

	char* WriteStandardSpecifier(StrTimeSpecifier specifier, int32_t data[], char *target) {
		// data contains [0] year, [1] month, [2] day, [3] hour, [4] minute, [5] second, [6] msec
		switch(specifier) {
		case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
			target = WritePadded2(target, data[2]);
			break;
		case StrTimeSpecifier::ABBREVIATED_MONTH_NAME: {
			auto &month_name = Date::MonthNamesAbbreviated[data[1] - 1];
			return WriteString(target, month_name);
		}
		case StrTimeSpecifier::FULL_MONTH_NAME: {
			auto &month_name = Date::MonthNames[data[1] - 1];
			return WriteString(target, month_name);
		}
		case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
			target = WritePadded2(target, data[1]);
			break;
		case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
			target = WritePadded2(target, data[0] % 100);
			break;
		case StrTimeSpecifier::YEAR_DECIMAL:
			if (data[0] >= 0 && data[0] <= 9999) {
				target = WritePadded(target, data[0], 4);
			} else {
				int32_t year = data[0];
				if (data[0] < 0) {
					*target = '-';
					year = -year;
					target++;
				}
				auto len = NumericHelper::UnsignedLength<uint32_t>(year);
				NumericHelper::FormatUnsigned(year, target + len);
				target += len;
			}
			break;
		case StrTimeSpecifier::HOUR_24_PADDED: {
			target = WritePadded2(target, data[3]);
			break;
		}
		case StrTimeSpecifier::HOUR_12_PADDED: {
			int hour = data[3] % 12;
			if (hour == 0) {
				hour = 12;
			}
			target = WritePadded2(target, hour);
			break;
		}
		case StrTimeSpecifier::AM_PM:
			*target++ = data[3] >= 12 ? 'P' : 'A';
			*target++ = 'M';
			break;
		case StrTimeSpecifier::MINUTE_PADDED: {
			target = WritePadded2(target, data[4]);
			break;
		}
		case StrTimeSpecifier::SECOND_PADDED:
			target = WritePadded2(target, data[5]);
			break;
		case StrTimeSpecifier::MICROSECOND_PADDED:
			target = WritePadded(target, data[6] * 1000, 6);
			break;
		case StrTimeSpecifier::UTC_OFFSET:
		case StrTimeSpecifier::TZ_NAME:
			// always empty for now, FIXME when we have timestamp with tz
			break;
		case StrTimeSpecifier::DAY_OF_MONTH: {
			target = Write2(target, data[2] % 100);
			break;
		}
		case StrTimeSpecifier::MONTH_DECIMAL: {
			target = Write2(target, data[1]);
			break;
		}
		case StrTimeSpecifier::YEAR_WITHOUT_CENTURY: {
			target = Write2(target, data[0] % 100);
			break;
		}
		case StrTimeSpecifier::HOUR_24_DECIMAL: {
			target = Write2(target, data[3]);
			break;
		}
		case StrTimeSpecifier::HOUR_12_DECIMAL: {
			int hour = data[3] % 12;
			if (hour == 0) {
				hour = 12;
			}
			target = Write2(target, hour);
			break;
		}
		case StrTimeSpecifier::MINUTE_DECIMAL: {
			target = Write2(target, data[4]);
			break;
		}
		case StrTimeSpecifier::SECOND_DECIMAL: {
			target = Write2(target, data[5]);
			break;
		}
		default:
			throw NotImplementedException("Unimplemented specifier for WriteStandardSpecifier in strftime");
		}
		return target;
	}

	void FormatString(date_t date, int32_t data[7], char *target) {
		idx_t i;
		for(i = 0; i < specifiers.size(); i++) {
			// first copy the current literal
			memcpy(target, literals[i].c_str(), literals[i].size());
			target += literals[i].size();
			// now copy the specifier
			if (is_date_specifier[i]) {
				target = WriteDateSpecifier(specifiers[i], date, target);
			} else {
				target = WriteStandardSpecifier(specifiers[i], data, target);
			}
		}
		// copy the final literal into the target
		memcpy(target, literals[i].c_str(), literals[i].size());

	}

	void FormatString(date_t date, time_t time, char *target) {
		int32_t data[7]; // year, month, day, hour, min, sec, msec
		Date::Convert(date, data[0], data[1], data[2]);
		Time::Convert(time, data[3], data[4], data[5], data[6]);

		FormatString(date, data, target);
	}
};

string ParseFormatSpecifier(string format_string, StrTimeFormat &format) {
	format.constant_size = 0;
	idx_t pos = 0;
	string current_literal;
	for(idx_t i = 0; i < format_string.size(); i++) {
		if (format_string[i] == '%') {
			if (i + 1 == format_string.size()) {
				return "Trailing format character %";
			}
			if (i > pos) {
				// push the previous string to the current literal
				current_literal += format_string.substr(pos, i - pos);
			}
			char format_char = format_string[++i];
			if (format_char == '%') {
				// special case: %%
				// set the pos for the next literal and continue
				pos = i;
				continue;
			}
			StrTimeSpecifier specifier;
			if (format_char == '-' && i + 1 < format_string.size()) {
				format_char = format_string[++i];
				switch(format_char) {
				case 'd':
					specifier = StrTimeSpecifier::DAY_OF_MONTH;
					break;
				case 'm':
					specifier = StrTimeSpecifier::MONTH_DECIMAL;
					break;
				case 'y':
					specifier = StrTimeSpecifier::YEAR_WITHOUT_CENTURY;
					break;
				case 'H':
					specifier = StrTimeSpecifier::HOUR_24_DECIMAL;
					break;
				case 'I':
					specifier = StrTimeSpecifier::HOUR_12_DECIMAL;
					break;
				case 'M':
					specifier = StrTimeSpecifier::MINUTE_DECIMAL;
					break;
				case 'S':
					specifier = StrTimeSpecifier::SECOND_DECIMAL;
					break;
				case 'j':
					specifier = StrTimeSpecifier::DAY_OF_YEAR_DECIMAL;
					break;
				default:
					return "Unrecognized format for strftime/strptime: %-" + string(format_char, 1);
				}
			} else {
				switch(format_char) {
				case 'a':
					specifier = StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME;
					break;
				case 'A':
					specifier = StrTimeSpecifier::FULL_WEEKDAY_NAME;
					break;
				case 'w':
					specifier = StrTimeSpecifier::WEEKDAY_DECIMAL;
					break;
				case 'd':
					specifier = StrTimeSpecifier::DAY_OF_MONTH_PADDED;
					break;
				case 'h':
				case 'b':
					specifier = StrTimeSpecifier::ABBREVIATED_MONTH_NAME;
					break;
				case 'B':
					specifier = StrTimeSpecifier::FULL_MONTH_NAME;
					break;
				case 'm':
					specifier = StrTimeSpecifier::MONTH_DECIMAL_PADDED;
					break;
				case 'y':
					specifier = StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED;
					break;
				case 'Y':
					specifier = StrTimeSpecifier::YEAR_DECIMAL;
					break;
				case 'H':
					specifier = StrTimeSpecifier::HOUR_24_PADDED;
					break;
				case 'I':
					specifier = StrTimeSpecifier::HOUR_12_PADDED;
					break;
				case 'p':
					specifier = StrTimeSpecifier::AM_PM;
					break;
				case 'M':
					specifier = StrTimeSpecifier::MINUTE_PADDED;
					break;
				case 'S':
					specifier = StrTimeSpecifier::SECOND_PADDED;
					break;
				case 'f':
					specifier = StrTimeSpecifier::MICROSECOND_PADDED;
					break;
				case 'z':
					specifier = StrTimeSpecifier::UTC_OFFSET;
					break;
				case 'Z':
					specifier = StrTimeSpecifier::TZ_NAME;
					break;
				case 'j':
					specifier = StrTimeSpecifier::DAY_OF_YEAR_PADDED;
					break;
				case 'U':
					specifier = StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST;
					break;
				case 'W':
					specifier = StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST;
					break;
				case 'c':
				case 'x':
				case 'X': {
					string subformat;
					if (format_char == 'c') {
						// %c: Locale’s appropriate date and time representation.
						// we push the ISO timestamp representation here
						subformat = "%Y-%m-%d %H:%M:%S";
					} else if (format_char == 'x') {
						// %x - Locale’s appropriate date representation.
						// we push the ISO date format here
						subformat = "%Y-%m-%d";
					} else if (format_char == 'X') {
						// %X - Locale’s appropriate time representation.
						// we push the ISO time format here
						subformat = "%H:%M:%S";
					}
					// parse the subformat in a separate format specifier
					StrfTimeFormat locale_format;
					string error = ParseFormatSpecifier(subformat, locale_format);
					assert(error.empty());
					// add the previous literal to the first literal of the subformat
					locale_format.literals[0] = move(current_literal) + locale_format.literals[0];
					// now push the subformat into the current format specifier
					for(idx_t i = 0; i < locale_format.specifiers.size(); i++) {
						format.AddFormatSpecifier(move(locale_format.literals[i]), locale_format.specifiers[i]);
					}
					pos = i + 1;
					continue;
				}
				default:
					return "Unrecognized format for strftime/strptime: %" + string(format_char, 1);
				}
			}
			format.AddFormatSpecifier(move(current_literal), specifier);
			pos = i + 1;
		}
	}
	// add the final literal
	if (pos < format_string.size()) {
		current_literal += format_string.substr(pos, format_string.size() - pos);
	}
	format.AddLiteral(move(current_literal));
	return string();
}

struct StrfTimeBindData : public FunctionData {
	StrfTimeBindData(StrfTimeFormat format) : format(move(format)) {}

	StrfTimeFormat format;

	unique_ptr<FunctionData> Copy() override {
		return make_unique<StrfTimeBindData>(format);
	}
};

static unique_ptr<FunctionData> strftime_bind_function(BoundFunctionExpression &expr, ClientContext &context) {
	if (!expr.children[1]->IsScalar()) {
		throw InvalidInputException("strftime format must be a constant");
	}
	Value options_str = ExpressionExecutor::EvaluateScalar(*expr.children[1]);
	StrfTimeFormat format;
	if (!options_str.is_null && options_str.type == TypeId::VARCHAR) {
		string error = ParseFormatSpecifier(options_str.str_value, format);
		if (!error.empty()) {
			throw InvalidInputException("Failed to parse format specifier %s: %s", options_str.str_value.c_str(), error.c_str());
		}
	}
	return make_unique<StrfTimeBindData>(format);
}

static void strftime_function_date(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (StrfTimeBindData &)*func_expr.bind_info;

	if (ConstantVector::IsNull(args.data[1])) {
		result.vector_type = VectorType::CONSTANT_VECTOR;
		ConstantVector::SetNull(result, true);
		return;
	}

	time_t time = 0;
	UnaryExecutor::Execute<date_t, string_t, true>(args.data[0], result, args.size(), [&](date_t date) {
		idx_t len = info.format.GetLength(date, time);
		string_t target = StringVector::EmptyString(result, len);
		info.format.FormatString(date, time, target.GetData());
		target.Finalize();
		return target;
	});
}

static void strftime_function_timestamp(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (StrfTimeBindData &)*func_expr.bind_info;

	if (ConstantVector::IsNull(args.data[1])) {
		result.vector_type = VectorType::CONSTANT_VECTOR;
		ConstantVector::SetNull(result, true);
		return;
	}

	UnaryExecutor::Execute<timestamp_t, string_t, true>(args.data[0], result, args.size(), [&](timestamp_t timestamp) {
		date_t date;
		dtime_t time;
		Timestamp::Convert(timestamp, date, time);
		idx_t len = info.format.GetLength(date, time);
		string_t target = StringVector::EmptyString(result, len);
		info.format.FormatString(date, time, target.GetData());
		target.Finalize();
		return target;
	});
}

void StrfTimeFun::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet strftime("strftime");

	strftime.AddFunction(ScalarFunction({SQLType::DATE, SQLType::VARCHAR}, SQLType::VARCHAR,
	                               strftime_function_date, false, strftime_bind_function));

	strftime.AddFunction(ScalarFunction({SQLType::TIMESTAMP, SQLType::VARCHAR}, SQLType::VARCHAR,
	                               strftime_function_timestamp, false, strftime_bind_function));

	set.AddFunction(strftime);
}

struct StrpTimeFormat : public StrTimeFormat {
	//! The full format specifier, for error messages
	string format_specifier;

	void AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) override {
		switch(specifier) {
		case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
		case StrTimeSpecifier::WEEKDAY_DECIMAL:
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
			throw NotImplementedException("Unimplemented specifier for strptime");
		default:
			break;
		}
		is_numeric.push_back(IsNumericSpecifier(specifier));
		StrTimeFormat::AddFormatSpecifier(move(preceding_literal), specifier);
	}

	bool IsNumericSpecifier(StrTimeSpecifier specifier) {
		switch(specifier) {
		case StrTimeSpecifier::WEEKDAY_DECIMAL:
		case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
		case StrTimeSpecifier::DAY_OF_MONTH:
		case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
		case StrTimeSpecifier::MONTH_DECIMAL:
		case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
		case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
		case StrTimeSpecifier::YEAR_DECIMAL:
		case StrTimeSpecifier::HOUR_24_PADDED:
		case StrTimeSpecifier::HOUR_24_DECIMAL:
		case StrTimeSpecifier::HOUR_12_PADDED:
		case StrTimeSpecifier::HOUR_12_DECIMAL:
		case StrTimeSpecifier::MINUTE_PADDED:
		case StrTimeSpecifier::MINUTE_DECIMAL:
		case StrTimeSpecifier::SECOND_PADDED:
		case StrTimeSpecifier::SECOND_DECIMAL:
		case StrTimeSpecifier::MICROSECOND_PADDED:
		case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
		case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
			return true;
		default:
			return false;
		}
	}

	enum class TimeSpecifierAMOrPM : uint8_t {
		TIME_SPECIFIER_NONE = 0,
		TIME_SPECIFIER_AM = 1,
		TIME_SPECIFIER_PM = 2
	};

	int32_t TryParseCollection(const char *data, idx_t &pos, idx_t size, string_t collection[], idx_t collection_count) {
		for(idx_t c = 0; c < collection_count; c++) {
			auto &entry = collection[c];
			auto entry_data = entry.GetData();
			auto entry_size = entry.GetSize();
			// check if this entry matches
			if (pos + entry_size > size) {
				// too big: can't match
				continue;
			}
			// compare the characters
			idx_t i;
			for(i = 0; i < entry_size; i++) {
				if (std::tolower(entry_data[i]) != std::tolower(data[pos + i])) {
					break;
				}
			}
			if (i == entry_size) {
				// full match
				pos += entry_size;
				return c;
			}
		}
		return -1;
	}

	//! Parses a timestamp using the given specifier
	bool Parse(string_t str, int32_t result_data[], string &error_message, idx_t &error_position) {
		// initialize the result
		result_data[0] = 1900;
		result_data[1] = 1;
		result_data[2] = 1;
		result_data[3] = 0;
		result_data[4] = 0;
		result_data[5] = 0;
		result_data[6] = 0;

		auto data = str.GetData();
		idx_t size = str.GetSize();
		// skip leading spaces
		while(std::isspace(*data)) {
			data++;
			size--;
		}
		idx_t pos = 0;
		TimeSpecifierAMOrPM ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_NONE;

		for(idx_t i = 0; ; i++) {
			// first compare the literal
			if (literals[i].size() > (size - pos) || memcmp(data + pos, literals[i].c_str(), literals[i].size()) != 0) {
				// literal does not match
				error_message = "Literal does not match, expected " + literals[i];
				error_position = pos;
				return false;
			}
			pos += literals[i].size();
			if (i == specifiers.size()) {
				break;
			}
			// now parse the specifier
			if (is_numeric[i]) {
				// numeric specifier: parse a number
				uint64_t number = 0;
				size_t start_pos = pos;
				while(pos < size && std::isdigit(data[pos])) {
					if (number > 1000000ULL) {
						// no number bigger than this is required anywhere
						error_message = "Number is out of range of format specifier";
						error_position = start_pos;
						return false;
					}
					number = number * 10 + data[pos] - '0';
					pos++;
				}
				if (pos == start_pos) {
					// expected a number here
					error_message = "Expected a number";
					error_position = start_pos;
					return false;
				}
				switch(specifiers[i]) {
				case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
				case StrTimeSpecifier::DAY_OF_MONTH:
					if (number < 1 || number > 31) {
						error_message = "Day out of range, expected a value between 1 and 31";
						error_position = start_pos;
						return false;
					}
					// day of the month
					result_data[2] = number;
					break;
				case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
				case StrTimeSpecifier::MONTH_DECIMAL:
					if (number < 1 || number > 12) {
						error_message = "Month out of range, expected a value between 1 and 12";
						error_position = start_pos;
						return false;
					}
					// month number
					result_data[1] = number;
					break;
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
					// year without century..
					// Python uses 69 as a crossover point (i.e. >= 69 is 19.., < 69 is 20..)
					if (number >= 100) {
						// %y only supports numbers between [0..99]
						error_message = "Year without century out of range, expected a value between 0 and 99";
						error_position = start_pos;
						return false;
					}
					if (number >= 69) {
						result_data[0] = 1900 + number;
					} else {
						result_data[0] = 2000 + number;
					}
					break;
				case StrTimeSpecifier::YEAR_DECIMAL:
					// year as full number
					result_data[0] = number;
					break;
				case StrTimeSpecifier::HOUR_24_PADDED:
				case StrTimeSpecifier::HOUR_24_DECIMAL:
					if (number >= 24) {
						error_message = "Hour out of range, expected a value between 0 and 23";
						error_position = start_pos;
						return false;
					}
					// hour as full number
					result_data[3] = number;
					break;
				case StrTimeSpecifier::HOUR_12_PADDED:
				case StrTimeSpecifier::HOUR_12_DECIMAL:
					if (number < 1 || number > 12) {
						error_message = "Hour12 out of range, expected a value between 1 and 12";
						error_position = start_pos;
						return false;
					}
					// 12-hour number: start off by just storing the number
					result_data[3] = number;
					break;
				case StrTimeSpecifier::MINUTE_PADDED:
				case StrTimeSpecifier::MINUTE_DECIMAL:
					if (number >= 60) {
						error_message = "Minutes out of range, expected a value between 0 and 59";
						error_position = start_pos;
						return false;
					}
					// minutes
					result_data[4] = number;
					break;
				case StrTimeSpecifier::SECOND_PADDED:
				case StrTimeSpecifier::SECOND_DECIMAL:
					if (number >= 60) {
						error_message = "Seconds out of range, expected a value between 0 and 59";
						error_position = start_pos;
						return false;
					}
					// seconds
					result_data[5] = number;
					break;
				case StrTimeSpecifier::MICROSECOND_PADDED:
					if (number >= 1000000ULL) {
						error_message = "Microseconds out of range, expected a value between 0 and 999999";
						error_position = start_pos;
						return false;
					}
					// microseconds
					result_data[6] = number * 1000;
					break;
				default:
					throw NotImplementedException("Unsupported specifier for strptime");
				}
			} else {
				switch(specifiers[i]) {
				case StrTimeSpecifier::AM_PM: {
					// parse the next 2 characters
					if (pos + 2 > size) {
						// no characters left to parse
						error_message = "Expected AM/PM";
						error_position = pos;
						return false;
					}
					char pa_char = std::tolower(data[pos]);
					char m_char = std::tolower(data[pos + 1]);
					if (m_char != 'm') {
						error_message = "Expected AM/PM";
						error_position = pos;
						return false;
					}
					if (pa_char == 'p') {
						ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_PM;
					} else if (pa_char == 'a') {
						ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_AM;
					} else {
						error_message = "Expected AM/PM";
						error_position = pos;
						return false;
					}
					pos += 2;
					break;
				}
				// we parse weekday names, but we don't use them as information
				case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
					if (TryParseCollection(data, pos, size, Date::DayNamesAbbreviated, 7) < 0) {
						error_message = "Expected an abbreviated day name (Mon, Tue, Wed, Thu, Fri, Sat, Sun)";
						error_position = pos;
						return false;
					}
					break;
				case StrTimeSpecifier::FULL_WEEKDAY_NAME:
					if (TryParseCollection(data, pos, size, Date::DayNames, 7) < 0) {
						error_message = "Expected a full day name (Monday, Tuesday, etc...)";
						error_position = pos;
						return false;
					}
					break;
				case StrTimeSpecifier::ABBREVIATED_MONTH_NAME: {
					int32_t month = TryParseCollection(data, pos, size, Date::MonthNamesAbbreviated, 12);
					if (month < 0) {
						error_message = "Expected an abbreviated month name (Jan, Feb, Mar, etc..)";
						error_position = pos;
						return false;
					}
					result_data[1] = month + 1;
					break;
				}
				case StrTimeSpecifier::FULL_MONTH_NAME: {
					int32_t month = TryParseCollection(data, pos, size, Date::MonthNames, 12);
					if (month < 0) {
						error_message = "Expected a full month name (January, February, etc...)";
						error_position = pos;
						return false;
					}
					result_data[1] = month + 1;
					break;
				}
				default:
					throw NotImplementedException("Unsupported specifier for strptime");
				}
			}
		}
		// skip trailing spaces
		while(std::isspace(data[pos])) {
			pos++;
		}
		if (pos != size) {
			error_message = "Full specifier did not match: trailing characters";
			error_position = pos;
			return false;
		}
		if (ampm != TimeSpecifierAMOrPM::TIME_SPECIFIER_NONE) {
			// fixme: adjust the hours based on the AM or PM specifier
			if (ampm == TimeSpecifierAMOrPM::TIME_SPECIFIER_AM) {
				// AM: 12AM=0, 1AM=1, 2AM=2, ..., 11AM=11
				if (result_data[3] == 12) {
					result_data[3] = 0;
				}
			} else {
				// PM: 12PM=12, 1PM=13, 2PM=14, ..., 11PM=23
				if (result_data[3] != 12) {
					result_data[3] += 12;
				}
			}
		}
		return true;
	}
};


struct StrpTimeBindData : public FunctionData {
	StrpTimeBindData(StrpTimeFormat format) : format(move(format)) {}

	StrpTimeFormat format;

	unique_ptr<FunctionData> Copy() override {
		return make_unique<StrpTimeBindData>(format);
	}
};

static unique_ptr<FunctionData> strptime_bind_function(BoundFunctionExpression &expr, ClientContext &context) {
	if (!expr.children[1]->IsScalar()) {
		throw InvalidInputException("strftime format must be a constant");
	}
	Value options_str = ExpressionExecutor::EvaluateScalar(*expr.children[1]);
	StrpTimeFormat format;
	if (!options_str.is_null && options_str.type == TypeId::VARCHAR) {
		format.format_specifier = options_str.str_value;
		string error = ParseFormatSpecifier(options_str.str_value, format);
		if (!error.empty()) {
			throw InvalidInputException("Failed to parse format specifier %s: %s", options_str.str_value.c_str(), error.c_str());
		}
	}
	return make_unique<StrpTimeBindData>(format);
}

static string FormatError(string input, idx_t position) {
	if (position == INVALID_INDEX) {
		return string();
	}
	return input + "\n" + string(position, ' ') + "^";
}

static void strptime_function(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (StrpTimeBindData &)*func_expr.bind_info;

	if (ConstantVector::IsNull(args.data[1])) {
		result.vector_type = VectorType::CONSTANT_VECTOR;
		ConstantVector::SetNull(result, true);
		return;
	}
	string error_message;
	idx_t error_position = INVALID_INDEX;
	int32_t result_data[7];
	UnaryExecutor::Execute<string_t, timestamp_t, true>(args.data[0], result, args.size(), [&](string_t input) {
		if (!info.format.Parse(input, result_data, error_message, error_position)) {
			throw InvalidInputException("Could not parse string \"%s\" according to format specifier \"%s\"\n%s\nError: %s",
			    input.GetData(),
				info.format.format_specifier.c_str(),
				FormatError(string(input.GetData(), input.GetSize()), error_position).c_str(),
				error_message.c_str());
		}
		date_t date = Date::FromDate(result_data[0], result_data[1], result_data[2]);
		dtime_t time = Time::FromTime(result_data[3], result_data[4], result_data[5], result_data[6]);
		return Timestamp::FromDatetime(date, time);
	});
}

void StrpTimeFun::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet strptime("strptime");

	strptime.AddFunction(ScalarFunction({SQLType::VARCHAR, SQLType::VARCHAR}, SQLType::TIMESTAMP,
	                               strptime_function, false, strptime_bind_function));

	set.AddFunction(strptime);
}


}