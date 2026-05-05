// b3dm.cpp
#include "b3dm.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <vector>

namespace
{
    std::uint32_t PadTo8(std::uint32_t n)
    {
        return (n + 7u) & ~7u;
    }

    void AppendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v)
    {
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    }

    void AppendBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t len)
    {
        const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + len);
    }

    std::string JsonEscape(const std::string& input)
    {
        std::string out;
        out.reserve(input.size() + 8);
        for (const char ch : input)
        {
            switch (ch)
            {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(ch); break;
            }
        }
        return out;
    }

    std::string BuildBatchTableJson(const std::map<std::string, std::string>& metadata)
    {
        if (metadata.empty())
        {
            return "{}";
        }

        std::string json = "{";
        bool first = true;
        for (const auto& entry : metadata)
        {
            if (!first)
            {
                json += ",";
            }
            first = false;
            json += "\"" + JsonEscape(entry.first) + "\":[\"" + JsonEscape(entry.second) + "\"]";
        }
        json += "}";
        return json;
    }

    /// B3DM header + padded JSON sections only (GLB payload appended separately for streaming).
    void BuildB3dmPrefixBytes(
        std::uint32_t glbByteLength,
        const std::map<std::string, std::string>& metadata,
        std::vector<std::uint8_t>& outPrefix)
    {
        const std::string ftJsonStorage = metadata.empty() ? "{}" : "{\"BATCH_LENGTH\":1}";
        const char* ftJson = ftJsonStorage.c_str();
        const std::uint32_t ftJsonLen = static_cast<std::uint32_t>(std::strlen(ftJson));
        const std::uint32_t ftJsonLenPadded = PadTo8(ftJsonLen);

        const std::string btJsonStorage = BuildBatchTableJson(metadata);
        const char* btJson = btJsonStorage.c_str();
        const std::uint32_t btJsonLen = static_cast<std::uint32_t>(std::strlen(btJson));
        const std::uint32_t btJsonLenPadded = PadTo8(btJsonLen);

        const std::uint32_t headerLen = 28;
        const std::uint32_t ftBinLen = 0;
        const std::uint32_t btBinLen = 0;

        const std::uint32_t totalByteLength =
            headerLen +
            ftJsonLenPadded + ftBinLen +
            btJsonLenPadded + btBinLen +
            glbByteLength;

        outPrefix.clear();
        outPrefix.reserve(static_cast<std::size_t>(totalByteLength - glbByteLength));

        outPrefix.push_back('b'); outPrefix.push_back('3'); outPrefix.push_back('d'); outPrefix.push_back('m');
        AppendU32LE(outPrefix, 1);
        AppendU32LE(outPrefix, totalByteLength);
        AppendU32LE(outPrefix, ftJsonLenPadded);
        AppendU32LE(outPrefix, ftBinLen);
        AppendU32LE(outPrefix, btJsonLenPadded);
        AppendU32LE(outPrefix, btBinLen);

        AppendBytes(outPrefix, ftJson, ftJsonLen);
        outPrefix.insert(outPrefix.end(), ftJsonLenPadded - ftJsonLen, static_cast<std::uint8_t>(' '));

        AppendBytes(outPrefix, btJson, btJsonLen);
        outPrefix.insert(outPrefix.end(), btJsonLenPadded - btJsonLen, static_cast<std::uint8_t>(' '));
    }

    bool StreamGlbFileToB3dmFile(
        const std::string& glbPath,
        const std::string& b3dmPath,
        const std::map<std::string, std::string>& metadata)
    {
        std::ifstream glbIn(glbPath, std::ios::binary);
        if (!glbIn)
        {
            return false;
        }

        glbIn.seekg(0, std::ios::end);
        const std::streampos endPos = glbIn.tellg();
        if (endPos <= std::streampos(0))
        {
            return false;
        }
        const std::uint64_t glbSize64 = static_cast<std::uint64_t>(endPos);
        if (glbSize64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return false;
        }
        const std::uint32_t glbSize = static_cast<std::uint32_t>(glbSize64);
        glbIn.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> prefix;
        BuildB3dmPrefixBytes(glbSize, metadata, prefix);

        std::ofstream b3dmOut(b3dmPath, std::ios::binary);
        if (!b3dmOut)
        {
            return false;
        }

        b3dmOut.write(reinterpret_cast<const char*>(prefix.data()),
                      static_cast<std::streamsize>(prefix.size()));
        if (!b3dmOut)
        {
            return false;
        }

        constexpr std::streamsize kChunk = static_cast<std::streamsize>(1024 * 1024);
        std::vector<char> chunk(static_cast<std::size_t>(kChunk));
        std::uint64_t copied = 0;
        while (glbIn && copied < glbSize64)
        {
            glbIn.read(chunk.data(), kChunk);
            const std::streamsize n = glbIn.gcount();
            if (n > 0)
            {
                b3dmOut.write(chunk.data(), n);
                if (!b3dmOut)
                {
                    return false;
                }
                copied += static_cast<std::uint64_t>(n);
            }
        }
        if (copied != glbSize64 || !glbIn.eof() || glbIn.bad() || !b3dmOut.flush())
        {
            return false;
        }
        return true;
    }
}

namespace B3dm
{
    bool WrapGlbFileToB3dmFile(const std::string& glbPath, const std::string& b3dmPath)
    {
        return WrapGlbFileToB3dmFile(glbPath, b3dmPath, {});
    }

    bool WrapGlbFileToB3dmFile(
        const std::string& glbPath,
        const std::string& b3dmPath,
        const std::map<std::string, std::string>& metadata)
    {
        return StreamGlbFileToB3dmFile(glbPath, b3dmPath, metadata);
    }
}
