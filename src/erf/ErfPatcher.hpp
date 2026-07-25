#pragma once

#include "erf/Archive.hpp"
#include "TslPatcher.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace neoerf {

enum class ArchivePatcherOperation {
    Install,
    Replace
};

struct ArchivePatcherChange {
    ArchivePatcherOperation operation = ArchivePatcherOperation::Install;
    std::string payloadName;
    std::string resourceKey;
    std::string resref;
    std::uint16_t restype = 0xFFFFu;
};

struct ArchivePatcherResult {
    neotsl::PatchProject project;
    std::vector<ArchivePatcherChange> changes;
    std::string targetArchivePath;

    std::size_t installCount() const noexcept;
    std::size_t replacementCount() const noexcept;
};

// TSLPatcher archive destinations are game-relative Windows paths, for example
// "Modules\\foo.mod". Absolute paths and parent traversal are deliberately
// rejected because changes.ini is installed relative to the game root.
std::string normalizePatcherArchiveDestination(std::string path);

// Builds stock-compatible [InstallList] instructions for adding/replacing
// resources inside a KotOR ERF/RIM/MOD archive. Resource deletion is not
// representable and is reported as unsupported.
ArchivePatcherResult diffArchivePatcher(ErfArchive& original,
                                        ErfArchive& modified,
                                        const std::string& targetArchivePath);

// Writes changes.ini and extracts every changed resource from the modified
// archive into the package folder. Existing output files are rejected unless
// overwriteExisting is true. Unsupported changes are rejected unless
// allowUnsupported is true.
void writeArchivePatcherPackage(const ArchivePatcherResult& result,
                                ErfArchive& modified,
                                const std::filesystem::path& outputDirectory,
                                bool allowUnsupported = false,
                                bool overwriteExisting = false);

} // namespace neoerf
