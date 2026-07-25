#include "erf/ErfPatcher.hpp"

#include "erf/Utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace neoerf {
namespace {

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string resourceKey(const Resource& resource) {
    return lowerAscii(resource.resref) + "#" + std::to_string(resource.restype);
}

std::string resourceDisplayKey(const Resource& resource, ResourceNameProfile profile) {
    return resource.resref + "." + Resource::res_type_to_string(resource.restype, profile);
}

std::string resourcePayloadName(const Resource& resource, ResourceNameProfile profile) {
    if (resource.resref.empty()) {
        throw std::runtime_error("Cannot generate an archive patch for a resource with an empty ResRef.");
    }
    if (resource.resref.find_first_of("/\\:") != std::string::npos || resource.resref == "." || resource.resref == "..") {
        throw std::runtime_error("Cannot generate a portable patch payload filename for resource ResRef: " + resource.resref);
    }

    const std::string extension = lowerAscii(Resource::res_type_to_string(resource.restype, profile));
    if (extension.empty() || extension.front() == '#') {
        throw std::runtime_error("TSLPatcher cannot infer an archive resource type from unknown extension " + extension +
                                 " for resource " + resource.resref + ".");
    }
    const std::string payload = lowerAscii(resource.resref) + "." + extension;
    if (lowerAscii(payload) == "changes.ini") {
        throw std::runtime_error("Resource payload collides with changes.ini: " + payload);
    }
    return payload;
}

void validateArchiveForPatcher(const ErfArchive& archive, const char* role) {
    if (!archive.loaded()) {
        throw std::runtime_error(std::string(role) + " archive is not loaded.");
    }
    if (archive.resource_type_profile() != ResourceNameProfile::KotOR) {
        throw std::runtime_error(std::string(role) +
                                 " archive is not using the KotOR resource profile. Stock TSLPatcher/HoloPatcher "
                                 "archive installation is supported here only for KotOR/KotOR II archives.");
    }
    if (archive.filename_based_resources()) {
        throw std::runtime_error(std::string(role) +
                                 " archive is filename-keyed. Dragon Age ERF V2/V3 archives are not compatible "
                                 "with stock TSLPatcher [InstallList] archive insertion.");
    }
    if (archive.extended_resrefs()) {
        throw std::runtime_error(std::string(role) +
                                 " archive uses extended 32-byte ResRefs, which stock KotOR TSLPatcher does not support.");
    }
    const auto format = archive.disk_format();
    if (format != ArchiveDiskFormat::ErfV1 && format != ArchiveDiskFormat::RimV1) {
        throw std::runtime_error(std::string(role) +
                                 " archive is not a KotOR ERF/RIM V1 archive supported by stock TSLPatcher.");
    }
}

std::map<std::string, const Resource*> resourcesByKey(const ErfArchive& archive) {
    std::map<std::string, const Resource*> out;
    for (const auto& resource : archive.resources()) {
        const std::string key = resourceKey(resource);
        if (!out.emplace(key, &resource).second) {
            throw std::runtime_error("Archive contains a duplicate resource key: " +
                                     resourceDisplayKey(resource, archive.resource_type_profile()));
        }
    }
    return out;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<unsigned long long> sequence{0};
        std::error_code ec;
        const auto root = std::filesystem::temp_directory_path(ec);
        if (ec) {
            throw std::runtime_error("Unable to locate the temporary directory: " + ec.message());
        }
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        for (unsigned attempt = 0; attempt < 100u; ++attempt) {
            path_ = root / ("neoerf-patcher-" + std::to_string(stamp) + "-" +
                            std::to_string(sequence.fetch_add(1)) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path_, ec)) {
                return;
            }
            if (ec && ec != std::errc::file_exists) {
                throw std::runtime_error("Unable to create a temporary archive comparison directory: " + ec.message());
            }
            ec.clear();
        }
        throw std::runtime_error("Unable to create a unique temporary archive comparison directory.");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

bool filesEqual(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const auto aSize = std::filesystem::file_size(a, ec);
    if (ec) throw std::runtime_error("Unable to inspect extracted resource " + a.string() + ": " + ec.message());
    const auto bSize = std::filesystem::file_size(b, ec);
    if (ec) throw std::runtime_error("Unable to inspect extracted resource " + b.string() + ": " + ec.message());
    if (aSize != bSize) return false;

    std::ifstream left(a, std::ios::binary);
    std::ifstream right(b, std::ios::binary);
    if (!left || !right) {
        throw std::runtime_error("Unable to reopen extracted resources for comparison.");
    }
    constexpr std::size_t bufferSize = 64u * 1024u;
    char leftBuffer[bufferSize];
    char rightBuffer[bufferSize];
    while (left && right) {
        left.read(leftBuffer, static_cast<std::streamsize>(bufferSize));
        right.read(rightBuffer, static_cast<std::streamsize>(bufferSize));
        const auto leftCount = left.gcount();
        const auto rightCount = right.gcount();
        if (leftCount != rightCount) return false;
        if (!std::equal(leftBuffer, leftBuffer + leftCount, rightBuffer)) return false;
    }
    return true;
}

bool payloadsEqual(ErfArchive& original,
                   const Resource& originalResource,
                   ErfArchive& modified,
                   const Resource& modifiedResource,
                   const std::filesystem::path& temporaryRoot,
                   std::size_t ordinal) {
    if (originalResource.data_size != modifiedResource.data_size) return false;
    const auto originalPath = temporaryRoot / ("original-" + std::to_string(ordinal) + ".bin");
    const auto modifiedPath = temporaryRoot / ("modified-" + std::to_string(ordinal) + ".bin");
    original.get_resource(originalResource.resref, originalResource.restype, originalPath);
    modified.get_resource(modifiedResource.resref, modifiedResource.restype, modifiedPath);
    return filesEqual(originalPath, modifiedPath);
}

void ensureOutputIsSafe(const std::filesystem::path& outputDirectory,
                        const ArchivePatcherResult& result,
                        bool overwriteExisting) {
    if (outputDirectory.empty()) {
        throw std::runtime_error("TSLPatcher package output directory is empty.");
    }
    std::set<std::string> generated;
    generated.insert("changes.ini");
    for (const auto& change : result.changes) {
        const std::string lower = lowerAscii(change.payloadName);
        if (!generated.insert(lower).second) {
            throw std::runtime_error("Multiple archive changes would generate the same payload filename: " +
                                     change.payloadName);
        }
    }
    if (overwriteExisting) return;
    for (const auto& name : generated) {
        std::error_code ec;
        if (std::filesystem::exists(outputDirectory / name, ec) && !ec) {
            throw std::runtime_error("Refusing to overwrite existing generated package file: " +
                                     (outputDirectory / name).string());
        }
    }
}

} // namespace

std::size_t ArchivePatcherResult::installCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [](const auto& change) {
        return change.operation == ArchivePatcherOperation::Install;
    }));
}

std::size_t ArchivePatcherResult::replacementCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [](const auto& change) {
        return change.operation == ArchivePatcherOperation::Replace;
    }));
}

std::string normalizePatcherArchiveDestination(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(path.begin());
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();
    if (path.empty()) throw std::runtime_error("Target archive path must not be empty.");
    if (path.front() == '\\' || (path.size() >= 2u && path[1] == ':')) {
        throw std::runtime_error("Target archive path must be relative to the game folder: " + path);
    }

    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find('\\', start);
        const std::string part = path.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
        if (part.empty() || part == "." || part == "..") {
            throw std::runtime_error("Target archive path contains an invalid empty/current/parent component: " + path);
        }
        if (part.find(':') != std::string::npos) {
            throw std::runtime_error("Target archive path contains an invalid colon: " + path);
        }
        parts.push_back(part);
        if (separator == std::string::npos) break;
        start = separator + 1u;
    }

    std::ostringstream normalized;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) normalized << '\\';
        normalized << parts[i];
    }
    return normalized.str();
}

ArchivePatcherResult diffArchivePatcher(ErfArchive& original,
                                        ErfArchive& modified,
                                        const std::string& targetArchivePath) {
    validateArchiveForPatcher(original, "Original");
    validateArchiveForPatcher(modified, "Modified");
    if (original.disk_format() != modified.disk_format()) {
        throw std::runtime_error("Original and modified archives use different native archive layouts.");
    }
    if (lowerAscii(original.file_type()) != lowerAscii(modified.file_type())) {
        throw std::runtime_error("Original and modified archives have different file types (" + original.file_type() +
                                 " versus " + modified.file_type() + ").");
    }

    ArchivePatcherResult result;
    result.targetArchivePath = normalizePatcherArchiveDestination(targetArchivePath);
    result.project.add("InstallList", "install_folder0", result.targetArchivePath);

    const auto originalByKey = resourcesByKey(original);
    const auto modifiedByKey = resourcesByKey(modified);
    TemporaryDirectory temporary;
    std::set<std::string> payloadNames;
    std::size_t comparisonOrdinal = 0;

    for (const auto& [key, originalResource] : originalByKey) {
        if (modifiedByKey.find(key) == modifiedByKey.end()) {
            result.project.unsupported.push_back(
                "Resource deletion is not representable by stock TSLPatcher [InstallList]: " +
                resourceDisplayKey(*originalResource, original.resource_type_profile()));
        }
    }

    for (const auto& [key, modifiedResource] : modifiedByKey) {
        const auto originalIt = originalByKey.find(key);
        ArchivePatcherOperation operation = ArchivePatcherOperation::Install;
        if (originalIt != originalByKey.end()) {
            if (payloadsEqual(original, *originalIt->second, modified, *modifiedResource,
                              temporary.path(), comparisonOrdinal++)) {
                continue;
            }
            operation = ArchivePatcherOperation::Replace;
        }

        const std::string payloadName = resourcePayloadName(*modifiedResource, modified.resource_type_profile());
        if (!payloadNames.insert(lowerAscii(payloadName)).second) {
            throw std::runtime_error("Archive changes would produce duplicate payload filename: " + payloadName);
        }
        result.changes.push_back({operation, payloadName, key, modifiedResource->resref, modifiedResource->restype});
    }

    std::size_t installIndex = 0;
    std::size_t replaceIndex = 0;
    for (const auto& change : result.changes) {
        if (change.operation == ArchivePatcherOperation::Install) {
            result.project.add("install_folder0", "Install" + std::to_string(installIndex++), change.payloadName);
        } else {
            result.project.add("install_folder0", "Replace" + std::to_string(replaceIndex++), change.payloadName);
        }
    }

    if (result.replacementCount() > 0) {
        result.project.warnings.push_back(
            "ReplaceN installs overwrite complete resources inside the target archive. For GFF, DLG, or JRL resources, "
            "use the corresponding semantic patch exporter when compatibility with other mods is required.");
    }
    if (result.changes.empty() && result.project.unsupported.empty()) {
        throw std::runtime_error("The archives contain no resource additions or payload changes.");
    }
    return result;
}

void writeArchivePatcherPackage(const ArchivePatcherResult& result,
                                ErfArchive& modified,
                                const std::filesystem::path& outputDirectory,
                                bool allowUnsupported,
                                bool overwriteExisting) {
    validateArchiveForPatcher(modified, "Modified");
    if (!allowUnsupported) neotsl::throwIfUnsupported(result.project);
    ensureOutputIsSafe(outputDirectory, result, overwriteExisting);

    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec) {
        throw std::runtime_error("Unable to create TSLPatcher package directory: " + outputDirectory.string() +
                                 ": " + ec.message());
    }

    std::vector<std::filesystem::path> written;
    try {
        for (const auto& change : result.changes) {
            const auto output = outputDirectory / change.payloadName;
            modified.get_resource(change.resref, change.restype, output);
            written.push_back(output);
        }
        neotsl::writeIniFile(result.project, outputDirectory / "changes.ini", true);
        written.push_back(outputDirectory / "changes.ini");
    } catch (...) {
        if (!overwriteExisting) {
            for (const auto& path : written) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
        throw;
    }
}

} // namespace neoerf
