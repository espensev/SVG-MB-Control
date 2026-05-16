#include "csv_util.h"

#include <cmath>
#include <ios>

namespace svg_mb_control {

std::string CsvEscape(std::string_view text) {
    bool needs_quotes = false;
    for (const char ch : text) {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return std::string(text);
    }

    std::string output;
    output.reserve(text.size() + 4u);
    output.push_back('"');
    for (const char ch : text) {
        if (ch == '"') {
            output += "\"\"";
        } else {
            output.push_back(ch);
        }
    }
    output.push_back('"');
    return output;
}

void AppendCsvString(std::ostringstream& csv, std::string_view text) {
    csv << CsvEscape(text);
}

void AppendCsvDouble(std::ostringstream& csv, double value, int precision) {
    if (std::isnan(value)) {
        return;
    }
    const std::streamsize old_precision = csv.precision();
    const auto old_flags = csv.flags();
    csv.setf(std::ios::fixed, std::ios::floatfield);
    csv.precision(precision);
    csv << value;
    csv.flags(old_flags);
    csv.precision(old_precision);
}

void AppendCsvBool(std::ostringstream& csv, bool value) {
    csv << (value ? "true" : "false");
}

void AppendCsvInt32WhenAvailable(std::ostringstream& csv, std::int32_t value) {
    if (value >= 0) {
        csv << value;
    }
}

}  // namespace svg_mb_control
