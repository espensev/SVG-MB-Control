#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace svg_mb_control {

// RFC 4180 style field escaping: wraps the value in double quotes and doubles
// embedded quotes when the value contains a quote, comma, CR, or LF.
std::string CsvEscape(std::string_view text);

void AppendCsvString(std::ostringstream& csv, std::string_view text);

// Appends a fixed-precision double. Writes nothing when value is NaN.
void AppendCsvDouble(std::ostringstream& csv, double value, int precision = 3);

void AppendCsvBool(std::ostringstream& csv, bool value);

// Appends the value only when it is non-negative; negative is treated as
// "not available" and writes nothing.
void AppendCsvInt32WhenAvailable(std::ostringstream& csv, std::int32_t value);

}  // namespace svg_mb_control
