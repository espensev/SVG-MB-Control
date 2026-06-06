#include "csv_util.h"

#include <cmath>
#include <ios>

namespace svg_mb_control {

namespace {

bool CsvNeedsQuotes(std::string_view text) {
    for (const char ch : text) {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
            return true;
        }
    }
    return false;
}

void AppendRaw(std::ostringstream& csv, std::string_view text) {
    if (text.empty()) {
        return;
    }
    csv.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}  // namespace

std::string CsvEscape(std::string_view text) {
    const bool needs_quotes = CsvNeedsQuotes(text);
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
    if (!CsvNeedsQuotes(text)) {
        AppendRaw(csv, text);
        return;
    }
    csv << '"';
    for (const char ch : text) {
        if (ch == '"') {
            csv << "\"\"";
        } else {
            csv << ch;
        }
    }
    csv << '"';
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
