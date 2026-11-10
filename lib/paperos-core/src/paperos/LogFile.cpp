#include "paperos/LogFile.h"

#include <cstdlib>
#include <vector>

namespace paperos {

bool debugEnabled(const Config& config) {
  const char* section = config.has("Developer", "debug") ? "Developer" : "developer";
  return config.getBool(section, "debug", false);
}

std::string LogFile::pathFor(int number) { return "/debug." + std::to_string(number) + ".log"; }

void LogFile::begin() {
  number_ = 0;
  size_ = 0;
  std::vector<DirEntry> entries;
  if (!fs_.list("/", entries)) return;
  bool found = false;
  for (const DirEntry& entry : entries) {
    const std::string& name = entry.name;
    // debug.<number>.log
    if (entry.isDirectory || name.size() < 11 || name.compare(0, 6, "debug.") != 0 ||
        name.compare(name.size() - 4, 4, ".log") != 0) {
      continue;
    }
    const std::string digits = name.substr(6, name.size() - 10);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
    const int number = std::atoi(digits.c_str());
    if (!found || number > number_) {
      number_ = number;
      size_ = entry.size;
      found = true;
    }
  }
}

void LogFile::add(const std::string& line) {
  if (pending_.size() + line.size() + 1 > kMaxPendingBytes) return;  // card gone; drop
  pending_ += line;
  if (line.empty() || line.back() != '\n') pending_ += '\n';
}

bool LogFile::flush() {
  if (pending_.empty()) return true;
  if (size_ > 0 && size_ + pending_.size() > maxBytes_) {
    ++number_;
    size_ = 0;
  }
  std::unique_ptr<File> file = fs_.open(path().c_str(), FileMode::Append);
  if (!file) return false;
  const size_t written = file->write(pending_.data(), pending_.size());
  size_ += written;
  pending_.erase(0, written);
  return pending_.empty();
}

}  // namespace paperos
