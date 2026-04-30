// b3dm.cpp
#include "b3dm.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>


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
}

namespace B3dm
{
    static std::vector<std::uint8_t> WrapGlbBytesWithMetadata(
        const std::vector<std::uint8_t>& glbBytes,
        const std::map<std::string, std::string>& metadata)
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

        const std::uint32_t byteLength =
            headerLen +
            ftJsonLenPadded + ftBinLen +
            btJsonLenPadded + btBinLen +
            static_cast<std::uint32_t>(glbBytes.size());

        std::vector<std::uint8_t> out;
        out.reserve(byteLength);

        // Header
        out.push_back('b'); out.push_back('3'); out.push_back('d'); out.push_back('m'); // magic
        AppendU32LE(out, 1);            // version
        AppendU32LE(out, byteLength);   // byteLength
        AppendU32LE(out, ftJsonLenPadded); // featureTableJsonByteLength
        AppendU32LE(out, ftBinLen);        // featureTableBinaryByteLength
        AppendU32LE(out, btJsonLenPadded); // batchTableJsonByteLength
        AppendU32LE(out, btBinLen);        // batchTableBinaryByteLength

        // Feature table JSON + padding (spaces)
        AppendBytes(out, ftJson, ftJsonLen);
        out.insert(out.end(), ftJsonLenPadded - ftJsonLen, static_cast<std::uint8_t>(' '));

        // Batch table JSON + padding (spaces)
        AppendBytes(out, btJson, btJsonLen);
        out.insert(out.end(), btJsonLenPadded - btJsonLen, static_cast<std::uint8_t>(' '));

        // GLB bytes
        AppendBytes(out, glbBytes.data(), glbBytes.size());

        return out;
    }

    std::vector<std::uint8_t> WrapGlbBytes(const std::vector<std::uint8_t>& glbBytes)
    {
        return WrapGlbBytesWithMetadata(glbBytes, {});
    }

    std::vector<std::uint8_t> ReadFileBytes(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return {};

        f.seekg(0, std::ios::end);
        std::streamsize size = f.tellg();
        f.seekg(0, std::ios::beg);

        if (size <= 0)
            return {};

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        f.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!f)
            return {};

        return bytes;
    }

    bool WriteBytesToFile(const std::string& path, const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream f(path, std::ios::binary);
        if (!f)
            return false;

        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(f);
    }

    bool WrapGlbFileToB3dmFile(const std::string& glbPath, const std::string& b3dmPath)
    {
        return WrapGlbFileToB3dmFile(glbPath, b3dmPath, {});
    }

    bool WrapGlbFileToB3dmFile(
        const std::string& glbPath,
        const std::string& b3dmPath,
        const std::map<std::string, std::string>& metadata)
    {
        //std::cout << "Reading GLB file: " << glbPath << "\n";

        std::vector<std::uint8_t> glb = ReadFileBytes(glbPath);
        if (glb.empty())
            return false;

        std::vector<std::uint8_t> b3dm = WrapGlbBytesWithMetadata(glb, metadata);

        //std::cout << "Writing B3DM file: " << b3dmPath << "\n";

        return WriteBytesToFile(b3dmPath, b3dm);
    }
}
