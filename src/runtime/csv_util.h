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

// Field-level helpers: each emits a leading comma followed by the formatted
// value (or empty when the gated variant's condition is false). They collapse
// the recurring `csv << ','; if (cond) { Append...(csv, value); }` pattern in
// the CSV row builders without changing the emitted bytes.
template <typename T>
void AppendCsvField(std::ostringstream& csv, const T& value) {
    csv << ',' << value;
}

template <typename T>
void AppendCsvFieldIf(std::ostringstream& csv, bool condition, const T& value) {
    csv << ',';
    if (condition) {
        csv << value;
    }
}

inline void AppendCsvFieldString(std::ostringstream& csv,
                                 std::string_view text) {
    csv << ',';
    AppendCsvString(csv, text);
}

inline void AppendCsvFieldStringIf(std::ostringstream& csv, bool condition,
                                   std::string_view text) {
    csv << ',';
    if (condition) {
        AppendCsvString(csv, text);
    }
}

inline void AppendCsvFieldBool(std::ostringstream& csv, bool value) {
    csv << ',';
    AppendCsvBool(csv, value);
}

inline void AppendCsvFieldBoolIf(std::ostringstream& csv, bool condition,
                                 bool value) {
    csv << ',';
    if (condition) {
        AppendCsvBool(csv, value);
    }
}

inline void AppendCsvFieldDouble(std::ostringstream& csv, double value,
                                 int precision = 3) {
    csv << ',';
    AppendCsvDouble(csv, value, precision);
}

inline void AppendCsvFieldDoubleIf(std::ostringstream& csv, bool condition,
                                   double value, int precision = 3) {
    csv << ',';
    if (condition) {
        AppendCsvDouble(csv, value, precision);
    }
}

inline void AppendCsvFieldInt32IfAvailable(std::ostringstream& csv,
                                           bool condition,
                                           std::int32_t value) {
    csv << ',';
    if (condition) {
        AppendCsvInt32WhenAvailable(csv, value);
    }
}

}  // namespace svg_mb_control
