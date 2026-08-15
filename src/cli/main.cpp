#include "erf/Archive.hpp"
#include "erf/ErfPatcher.hpp"
#include "erf/Utils.hpp"
#include "erf/Version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neoerf;

namespace {

void usage() {
    std::cout <<
        std::string("NeoERF ") + kNeoErfVersion + " command line\n"
        "\n"
        "Usage:\n"
        "  neoerf_cli [--game kotor|jade|nwn|nwn2|witcher|dao|da2] valid <archive>\n"
        "  neoerf_cli [--game kotor|jade|nwn|nwn2|witcher|dao|da2] list <archive> [filter-term]\n"
        "  neoerf_cli [--game kotor|jade|nwn|nwn2|witcher|dao|da2] search <archive> <term>\n"
        "  neoerf_cli [--game kotor|jade|nwn|nwn2|witcher|dao|da2] extract <archive> <destination-folder> [resref.ext ...]\n"
        "  neoerf_cli [--game kotor|jade|nwn|nwn2|witcher|dao|da2] create <archive> [--type ERF|ERF2|DAO|ERF22|DAO22|ERF22U|DAO22U|ERF3|DA2|MOD|HAK|SAV|NWM|RIM|RIMP|CRF] <file ...>\n"
        "  neoerf_cli [--game kotor|jade|nwn|nwn2|witcher|dao|da2] add <archive> [--output <archive>] [--no-replace] <file ...>\n"
        "  neoerf_cli [--game kotor] diff-tslpatcher <original-archive> <modified-archive> <output-dir> --target <game-relative-archive> [--ini installer.ini] [--allow-unsupported]\n"
        "  neoerf_cli [--game kotor|jade|nwn|nwn2|witcher|dao|da2] delete <archive> [--output <archive>] <resref.ext ...>\n"
        "\n"
        "Notes:\n"
        "  Unknown resource types can be addressed with #<number> extensions, e.g. foo.#1234.\n"
        "  --game selects the resource-extension table used for ResRef/type archives.\n"
        "  DAO ERF V2.0/V2.2 and DA2 ERF V3.0 archives are filename-keyed; extract/delete use full filenames.\n"
        "  ERF22/DAO22 creates compressed ERF V2.2; ERF22U/DAO22U creates uncompressed ERF V2.2.\n"
        "  diff-tslpatcher creates a complete [InstallList] package for KotOR ERF/RIM/MOD resources.\n"
        "  It supports resource additions and complete-resource replacements; deletion is not representable.\n"
        ;
}

ArchiveType parse_type(const std::string& text) {
    const auto upper = ascii_upper(text);
    if (upper == "MOD") return ArchiveType::MOD;
    if (upper == "HAK") return ArchiveType::HAK;
    if (upper == "ERF") return ArchiveType::ERF;
    if (upper == "SAV") return ArchiveType::SAV;
    if (upper == "NWM") return ArchiveType::NWM;
    if (upper == "RIM") return ArchiveType::RIM;
    if (upper == "ERF2" || upper == "ERFV2" || upper == "ERF_V2" || upper == "V2" || upper == "DAO" || upper == "DA") return ArchiveType::ERF_V2;
    if (upper == "ERF22" || upper == "ERF2.2" || upper == "ERFV22" || upper == "ERF_V2_2" || upper == "V22" || upper == "V2.2" || upper == "DAO22") return ArchiveType::ERF_V2_2;
    if (upper == "ERF22U" || upper == "ERF2.2U" || upper == "ERFV22U" || upper == "ERF_V2_2U" || upper == "V22U" || upper == "V2.2U" || upper == "DAO22U") return ArchiveType::ERF_V2_2_UNCOMPRESSED;
    if (upper == "ERF3" || upper == "ERFV3" || upper == "ERF_V3" || upper == "V3" || upper == "DA2" || upper == "DRAGONAGE2" || upper == "RIMP" || upper == "CRF") return ArchiveType::ERF_V3;
    throw std::runtime_error("Unsupported archive type: " + text);
}


ResourceNameProfile parse_resource_profile(const std::string& text) {
    const auto lower = ascii_lower(text);
    if (lower == "kotor" || lower == "k1" || lower == "k2") return ResourceNameProfile::KotOR;
    if (lower == "jade" || lower == "jadeempire" || lower == "je") return ResourceNameProfile::JadeEmpire;
    if (lower == "nwn" || lower == "nwn1" || lower == "neverwinter" || lower == "neverwinter1") return ResourceNameProfile::NeverwinterNights;
    if (lower == "nwn2" || lower == "neverwinter2") return ResourceNameProfile::NeverwinterNights2;
    if (lower == "witcher" || lower == "witcher1" || lower == "tw1") return ResourceNameProfile::Witcher;
    if (lower == "dao" || lower == "dragonage" || lower == "dragonageorigins" || lower == "daorigins") return ResourceNameProfile::DragonAgeOrigins;
    if (lower == "da2" || lower == "dragonage2" || lower == "dragonageii" || lower == "daii") return ResourceNameProfile::DragonAge2;
    throw std::runtime_error("Unsupported game/resource profile: " + text);
}


ResourceNameProfile parse_global_options(std::vector<std::string>& args) {
    ResourceNameProfile profile = ResourceNameProfile::KotOR;
    std::vector<std::string> out;
    out.reserve(args.size());
    if (!args.empty()) {
        out.push_back(args[0]);
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--game" || args[i] == "--profile") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(args[i] + " requires kotor, jade, nwn, nwn2, witcher, dao, or da2.");
            }
            profile = parse_resource_profile(args[++i]);
        } else {
            out.push_back(args[i]);
        }
    }
    args.swap(out);
    return profile;
}


std::string lower_ascii_local(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

struct Table {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

bool row_matches_filter(const Table& table, const std::vector<std::string>& row, const std::string& term) {
    const std::string needle = lower_ascii_local(term);
    for (const auto& column : table.columns) {
        if (lower_ascii_local(column).find(needle) != std::string::npos) return true;
    }
    for (const auto& cell : row) {
        if (lower_ascii_local(cell).find(needle) != std::string::npos) return true;
    }
    return false;
}

Table filter_rows(const Table& table, const std::string& term) {
    if (term.empty()) return table;
    Table out;
    out.columns = table.columns;
    for (const auto& row : table.rows) {
        if (row_matches_filter(table, row, term)) out.rows.push_back(row);
    }
    return out;
}

void print_table_tsv(const Table& table) {
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        if (c) std::cout << '\t';
        std::cout << table.columns[c];
    }
    std::cout << '\n';
    for (const auto& row : table.rows) {
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            if (c) std::cout << '\t';
            if (c < row.size()) std::cout << row[c];
        }
        std::cout << '\n';
    }
}

std::string resource_display_name(const Resource& res, ResourceNameProfile profile) {
    if (!res.filename.empty()) {
        return res.filename;
    }
    const std::string ext = res.extension(profile);
    return ext.empty() ? res.resref : (res.resref + "." + ext);
}

Table archive_to_table(const ErfArchive& archive, ResourceNameProfile profile) {
    Table table;
    table.columns = {"Name", "ResRef", "Extension", "TypeId", "Size", "Offset", "ResourceId", "File"};
    for (const auto& res : archive.resources()) {
        table.rows.push_back({
            resource_display_name(res, profile),
            res.resref,
            res.extension(profile),
            std::to_string(res.restype),
            std::to_string(res.data_size),
            std::to_string(res.data_offset),
            std::to_string(res.resid),
            std::string()
        });
    }
    return table;
}


void search_archive(const std::vector<std::string>& args, ResourceNameProfile profile) {
    if (args.size() != 4) {
        throw std::runtime_error("search requires <archive> <term>.");
    }
    ErfArchive archive;
    archive.set_resource_type_profile(profile);
    archive.load(args[2]);
    const auto active_profile = archive.resource_type_profile();
    print_table_tsv(filter_rows(archive_to_table(archive, active_profile), args[3]));
}

std::pair<std::string, std::uint16_t> parse_resource_name(const std::string& name, ResourceNameProfile profile) {
    const std::string resref = resource_stem_from_text(name);
    const auto type = Resource::string_to_res_type(extension_string(name), profile);
    if (type == 0xFFFFu) {
        throw std::runtime_error("Unsupported or missing resource type in: " + name);
    }
    return {resref, type};
}

void ensure_output_parent_exists(const std::filesystem::path& output) {
    const auto parent = output.parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        throw std::runtime_error("Unable to create extraction subdirectory: " + parent.string() + ": " + ec.message());
    }
}

void list_archive(const std::filesystem::path& archive_path, ResourceNameProfile profile, const std::string& filter_term = {}) {
    ErfArchive archive;
    archive.set_resource_type_profile(profile);
    archive.load(archive_path);
    std::cout << "Archive: " << archive.filename().string() << "\n";
    std::cout << "Type: " << archive.file_type() << "\n";
    if (archive.disk_format() == ArchiveDiskFormat::ErfV2_0) {
        std::cout << "Version: ERF V2.0 (UTF-16 filename table)\n";
    } else if (archive.disk_format() == ArchiveDiskFormat::ErfV2_2) {
        std::cout << "Version: ERF V2.2 (UTF-16 filename table, compression scheme "
                  << archive.compression_scheme() << ")\n";
    } else if (archive.disk_format() == ArchiveDiskFormat::ErfV3_0) {
        std::cout << "Version: ERF V3.0 (DA2 FNV hash table, compression scheme "
                  << archive.compression_scheme() << ")\n";
    } else {
        std::cout << "Version resrefs: " << (archive.extended_resrefs() ? "32-byte" : "16-byte") << "\n";
    }
    std::cout << "Resources: " << archive.count() << "\n";
    const auto active_profile = archive.resource_type_profile();
    auto table = archive_to_table(archive, active_profile);
    if (!filter_term.empty()) table = filter_rows(table, filter_term);
    print_table_tsv(table);
}


void extract_archive(const std::vector<std::string>& args, ResourceNameProfile profile) {
    if (args.size() < 4) {
        throw std::runtime_error("extract requires <archive> and <destination-folder>.");
    }
    ErfArchive archive;
    archive.set_resource_type_profile(profile);
    archive.load(args[2]);
    const std::filesystem::path dest = args[3];
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        throw std::runtime_error("Unable to create extraction destination folder: " + dest.string() + ": " + ec.message());
    }
    if (!directory_exists(dest)) {
        throw std::runtime_error("Extraction destination is not a directory: " + dest.string());
    }

    const auto active_profile = archive.resource_type_profile();
    if (args.size() == 4) {
        for (const auto& res : archive.resources()) {
            const std::string display_name = resource_display_name(res, active_profile);
            const std::string out_name = ascii_lower(display_name);
            const auto out_path = dest / std::filesystem::path(out_name);
            ensure_output_parent_exists(out_path);
            if (archive.filename_based_resources()) {
                archive.get_resource_by_name(display_name, out_path);
            } else {
                archive.get_resource(res.resref, res.restype, out_path);
            }
        }
    } else {
        for (std::size_t i = 4; i < args.size(); ++i) {
            if (archive.filename_based_resources()) {
                std::string out_name = args[i];
                std::replace(out_name.begin(), out_name.end(), '\\', '/');
                out_name = ascii_lower(out_name);
                const auto out_path = dest / std::filesystem::path(out_name);
                ensure_output_parent_exists(out_path);
                archive.get_resource_by_name(args[i], out_path);
            } else {
                const auto [resref, type] = parse_resource_name(args[i], active_profile);
                std::string out_name = ascii_lower(resref);
                const std::string ext = ascii_lower(Resource::res_type_to_string(type, active_profile));
                if (!ext.empty()) {
                    out_name += "." + ext;
                }
                const auto out_path = dest / out_name;
                ensure_output_parent_exists(out_path);
                archive.get_resource(resref, type, out_path);
            }
        }
    }
}

void create_archive(const std::vector<std::string>& args, ResourceNameProfile profile) {
    if (args.size() < 3) {
        throw std::runtime_error("create requires <archive>.");
    }
    const std::filesystem::path out_path = args[2];
    ArchiveType type = archive_type_from_extension(out_path);
    bool explicit_type = false;
    std::size_t first_file = 3;
    if (args.size() >= 5 && args[3] == "--type") {
        type = parse_type(args[4]);
        explicit_type = true;
        first_file = 5;
    }
    if (!explicit_type && profile == ResourceNameProfile::DragonAgeOrigins) {
        type = ArchiveType::ERF_V2;
    }
    if (!explicit_type && profile == ResourceNameProfile::DragonAge2) {
        type = ArchiveType::ERF_V3;
    }

    ErfArchive archive;
    archive.set_resource_type_profile(profile);
    archive.new_archive(out_path, type);
    for (std::size_t i = first_file; i < args.size(); ++i) {
        archive.add_resource(args[i], true);
    }
    archive.save(out_path);
}


void add_to_archive(const std::vector<std::string>& args, ResourceNameProfile profile) {
    if (args.size() < 4) {
        throw std::runtime_error("add requires <archive> and at least one file.");
    }
    std::filesystem::path archive_path = args[2];
    std::filesystem::path output_path;
    bool replace = true;
    std::vector<std::filesystem::path> files;

    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--output") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("--output requires a path.");
            }
            output_path = args[++i];
        } else if (args[i] == "--no-replace") {
            replace = false;
        } else {
            files.emplace_back(args[i]);
        }
    }
    if (files.empty()) {
        throw std::runtime_error("add requires at least one file.");
    }

    ErfArchive archive;
    archive.set_resource_type_profile(profile);
    archive.load(archive_path);
    for (const auto& file : files) {
        archive.add_resource(file, replace);
    }
    archive.save(output_path.empty() ? archive_path : output_path);
}

void export_archive_patcher(const std::vector<std::string>& args, ResourceNameProfile profile) {
    if (args.size() < 5) {
        throw std::runtime_error(
            "diff-tslpatcher requires <original-archive> <modified-archive> <output-dir> --target <game-relative-archive>.");
    }

    const std::filesystem::path originalPath = args[2];
    const std::filesystem::path modifiedPath = args[3];
    const std::filesystem::path outputDirectory = args[4];
    std::string targetArchivePath;
    std::filesystem::path iniFilename = "changes.ini";
    bool allowUnsupported = false;

    for (std::size_t i = 5; i < args.size(); ++i) {
        if (args[i] == "--target") {
            if (i + 1 >= args.size()) throw std::runtime_error("--target requires a game-relative archive path.");
            targetArchivePath = args[++i];
        } else if (args[i] == "--ini") {
            if (i + 1 >= args.size()) throw std::runtime_error("--ini requires a filename.");
            iniFilename = args[++i];
        } else if (args[i] == "--overwrite") {
            // Retained as a backwards-compatible no-op. Existing INIs are merged,
            // and conflicting payload files are never overwritten.
        } else if (args[i] == "--allow-unsupported") {
            allowUnsupported = true;
        } else if (args[i] == "--tslpatcher" || args[i] == "--holopatcher" || args[i] == "--package") {
            // The generated InstallList package is compatible with both installers.
        } else if (args[i] == "--fragment") {
            throw std::runtime_error(
                "NeoERF does not expose fragment mode because [InstallList] archive changes require resource payload files.");
        } else {
            throw std::runtime_error("Unknown diff-tslpatcher option: " + args[i]);
        }
    }
    if (targetArchivePath.empty()) {
        throw std::runtime_error("diff-tslpatcher requires --target <game-relative-archive>, for example Modules\\foo.mod.");
    }

    ErfArchive original;
    original.set_resource_type_profile(profile);
    original.load(originalPath);
    ErfArchive modified;
    modified.set_resource_type_profile(profile);
    modified.load(modifiedPath);

    auto result = diffArchivePatcher(original, modified, targetArchivePath);
    neotsl::printReport(result.project);
    const std::filesystem::path iniPath = iniFilename.is_absolute()
        ? iniFilename
        : outputDirectory / iniFilename;
    writeArchivePatcherPackageToIni(result, modified, iniPath, allowUnsupported);
    std::cout << "Wrote TSLPatcher/HoloPatcher archive resource package through " << iniPath.string() << "\n"
              << "  Installed resources: " << result.installCount() << "\n"
              << "  Replaced resources: " << result.replacementCount() << "\n";
}

void delete_from_archive(const std::vector<std::string>& args, ResourceNameProfile profile) {
    if (args.size() < 4) {
        throw std::runtime_error("delete requires <archive> and at least one resource name.");
    }
    std::filesystem::path archive_path = args[2];
    std::filesystem::path output_path;
    std::vector<std::string> names;

    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--output") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("--output requires a path.");
            }
            output_path = args[++i];
        } else {
            names.push_back(args[i]);
        }
    }
    if (names.empty()) {
        throw std::runtime_error("delete requires at least one resource name.");
    }

    ErfArchive archive;
    archive.set_resource_type_profile(profile);
    archive.load(archive_path);
    const auto active_profile = archive.resource_type_profile();
    for (const auto& name : names) {
        if (archive.filename_based_resources()) {
            archive.delete_resource_by_name(name);
        } else {
            const auto [resref, type] = parse_resource_name(name, active_profile);
            archive.delete_resource(resref, type);
        }
    }
    archive.save(output_path.empty() ? archive_path : output_path);
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv, argv + argc);
        const ResourceNameProfile profile = parse_global_options(args);
        if (args.size() < 2 || args[1] == "--help" || args[1] == "-h" || args[1] == "help") {
            usage();
            return args.size() < 2 ? 1 : 0;
        }
        if (args[1] == "--version" || args[1] == "-v" || args[1] == "version") {
            std::cout << "NeoERF " << kNeoErfVersion << "\n";
            return 0;
        }

        const std::string command = args[1];
        if (command == "valid") {
            if (args.size() != 3) {
                throw std::runtime_error("valid requires exactly one archive path.");
            }
            const bool ok = ErfArchive::is_valid_archive(args[2]);
            std::cout << (ok ? "valid" : "invalid") << "\n";
            return ok ? 0 : 2;
        }
        if (command == "list") {
            if (args.size() != 3 && args.size() != 4) {
                throw std::runtime_error("list requires <archive> and an optional filter term.");
            }
            list_archive(args[2], profile, args.size() == 4 ? args[3] : std::string());
            return 0;
        }
        if (command == "search") {
            search_archive(args, profile);
            return 0;
        }
        if (command == "extract") {
            extract_archive(args, profile);
            return 0;
        }
        if (command == "create") {
            create_archive(args, profile);
            return 0;
        }
        if (command == "add") {
            add_to_archive(args, profile);
            return 0;
        }
        if (command == "diff-tslpatcher" || command == "diff-holopatcher") {
            export_archive_patcher(args, profile);
            return 0;
        }
        if (command == "delete") {
            delete_from_archive(args, profile);
            return 0;
        }

        throw std::runtime_error("Unknown command: " + command);
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
