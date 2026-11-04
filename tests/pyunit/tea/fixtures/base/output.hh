
#pragma once
#include <fstream>
#include <string>

namespace gem5 {
struct OutputFile
{
    std::ofstream file;
    OutputFile(const std::string& path) : file(path) {}
    std::ostream* stream() { return &file; }
};
struct SimOut
{
    OutputFile* create(const std::string& path, bool, bool) { return new OutputFile(path); }
    void close(OutputFile* file) { delete file; }
};
inline SimOut simout;
}
