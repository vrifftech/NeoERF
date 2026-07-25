#pragma once

#include "erf/Utils.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace neoerf {

class ErfError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ArchiveType {
    MOD,
    HAK,
    ERF,
    SAV,
    NWM,
    RIM,
    ERF_V2,
    ERF_V2_2,
    ERF_V2_2_UNCOMPRESSED,
    ERF_V3
};

enum class ResourceNameProfile {
    KotOR,
    JadeEmpire,
    NeverwinterNights,
    NeverwinterNights2,
    Witcher,
    DragonAgeOrigins,
    DragonAge2
};

enum class ArchiveDiskFormat {
    ErfV1,
    RimV1,
    ErfV2_0,
    ErfV2_2,
    ErfV3_0
};


struct LocalizedString {
    std::uint32_t language_id = 0;
    std::string text;

    std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(text.size());
    }
};

struct Resource {
    bool extended = false;
    // Filename-keyed archives (Dragon Age ERF V2.x/V3.x) store the full
    // leaf filename in the TOC instead of a ResRef + type pair. This is empty
    // for ERF/RIM V1 archives.
    std::string filename;
    std::string resref;
    std::uint32_t resid = 0;
    std::uint16_t restype = 0xFFFF;
    std::uint16_t reserved_erf = 0;
    // Active RIM layout stores the reserved RIM field as a WORD on disk.
    std::uint32_t reserved_rim = 0;
    std::uint32_t data_offset = 0xFFFFFFFFu;
    // Logical/unpacked payload size. For compressed Dragon Age V2.2/V3.0
    // archives, packed_size stores the byte count present on disk.
    std::uint32_t data_size = 0;
    std::uint32_t packed_size = 0;

    // Dragon Age II ERF V3.0 stores lookup hashes and may strip filenames
    // from individual TOC entries.  When v3_filename_stripped is true,
    // filename contains a stable synthetic display/extraction name while
    // v3_name_hash/v3_type_hash preserve the on-disk lookup keys.
    std::uint64_t v3_name_hash = 0;
    std::uint32_t v3_type_hash = 0;
    bool v3_filename_stripped = false;

    // Backing storage for raw_resref/raw_resref32 property writes.  The
    // resource-name model keeps separate 16-byte and 32-byte buffers; setting one
    // raw property does not populate the other, and resref reads only the
    // buffer selected by the object's extended flag.
    bool raw_resref16_assigned = false;
    bool raw_resref32_assigned = false;
    std::array<char, 16> raw_resref16_storage{};
    std::array<char, 32> raw_resref32_storage{};

    std::array<char, 16> raw_resref() const;
    std::array<char, 32> raw_resref32() const;
    void set_raw_resref(const std::array<char, 16>& raw);
    void set_raw_resref32(const std::array<char, 32>& raw);

    std::string extension() const;
    std::string extension(ResourceNameProfile profile) const;

    static std::string res_type_to_string(std::uint16_t type);
    static std::string res_type_to_string(std::uint16_t type, ResourceNameProfile profile);
    static std::uint16_t string_to_res_type(std::string type);
    static std::uint16_t string_to_res_type(std::string type, ResourceNameProfile profile);
};

class ErfArchive {
public:
    ErfArchive();
    explicit ErfArchive(const std::filesystem::path& file_to_load);
    ~ErfArchive();

    ErfArchive(const ErfArchive&) = delete;
    ErfArchive& operator=(const ErfArchive&) = delete;
    ErfArchive(ErfArchive&&) noexcept = delete;
    ErfArchive& operator=(ErfArchive&&) noexcept = delete;

    void new_archive(const std::filesystem::path& filename, ArchiveType type);
    void load(const std::filesystem::path& filename);
    void save(std::filesystem::path filename = {}, std::string filetype_override = {});
    void reset(bool destroying = false);

    void add_resource(const std::filesystem::path& filename, bool replace, std::string save_as = {});
    void get_resource(const std::string& resref, std::uint16_t res_type, std::filesystem::path filename = {});
    void get_resource_by_name(const std::string& resource_name, std::filesystem::path filename = {});
    void delete_resource(const std::string& resref, std::uint16_t res_type);
    void delete_resource_by_name(const std::string& resource_name);
    bool resource_exists(const std::string& filename, bool check_new = false) const;
    bool resource_exists_by_name(const std::string& resource_name, bool check_new = false) const;

    static bool is_valid_archive(const std::filesystem::path& filename);

    void set_temp_path(std::filesystem::path path);
    void set_resource_type_profile(ResourceNameProfile profile) noexcept { resource_type_profile_ = profile; }
    ResourceNameProfile resource_type_profile() const noexcept { return resource_type_profile_; }

    const std::filesystem::path& temp_path() const noexcept { return temp_folder_path_; }
    const std::filesystem::path& temp_folder() const noexcept { return temp_folder_; }
    bool dirty() const noexcept { return dirty_; }
    bool loaded() const noexcept { return loaded_; }
    const std::filesystem::path& filename() const noexcept { return filename_; }
    std::string file_type() const;
    std::size_t count() const noexcept { return resources_.size(); }
    std::size_t count_new() const;
    const Resource& resource(std::size_t index) const;
    bool extended_resrefs() const noexcept { return resref32_; }
    bool filename_based_resources() const noexcept { return disk_format_ == ArchiveDiskFormat::ErfV2_0 || disk_format_ == ArchiveDiskFormat::ErfV2_2 || disk_format_ == ArchiveDiskFormat::ErfV3_0; }
    ArchiveDiskFormat disk_format() const noexcept { return disk_format_; }
    std::uint32_t archive_flags() const noexcept;
    std::uint32_t compression_scheme() const noexcept;
    std::uint32_t encryption_scheme() const noexcept;

    const std::vector<Resource>& resources() const noexcept { return resources_; }

private:
    struct Header {
        std::array<char, 4> filetype{{0, 0, 0, 0}};
        std::array<char, 4> version{{0, 0, 0, 0}};

        // ERF-family header fields.
        std::uint32_t locstringcount = 0;
        std::uint32_t locstringsize = 0;
        std::uint32_t entrycount = 0;
        std::uint32_t offsetlocstring = 0;
        std::uint32_t offsetkeylist = 0;
        std::uint32_t offsetreslist = 0;
        std::uint32_t buildyear = 0;
        std::uint32_t buildday = 0;
        std::uint32_t locstringstrref = 0xFFFFFFFFu;
        std::array<std::uint8_t, 116> erf_reserved{};

        // ERF V2.x (Dragon Age: Origins) stores count/year/day/unknown after a
        // UTF-16LE magic string. V2.2 adds archive-wide flags/module/password
        // fields and packed/unpacked sizes in each TOC row.
        std::uint32_t v2_unknown = 0xFFFFFFFFu;
        std::uint32_t v2_flags = 0;
        std::uint32_t v2_module_id = 0;
        std::array<std::uint8_t, 16> v2_password_digest{};

        // ERF V3.0 (Dragon Age II) uses a UTF-16LE magic string, an ASCII
        // filename string table, hashed TOC entries, and optional compression.
        std::uint32_t v3_string_table_size = 0;
        std::uint32_t v3_flags = 0;
        std::uint32_t v3_module_id = 0;
        std::array<std::uint8_t, 16> v3_password_digest{};

        // RIM-family header fields.
        std::uint32_t rim_unknown = 0;
        std::array<std::uint8_t, 100> rim_reserved{};

        bool is_rim() const;
    };

    const Resource* find_resource(const std::string& resref, std::uint16_t res_type) const;
    Resource* find_resource(const std::string& resref, std::uint16_t res_type);
    const Resource* find_resource_by_name(const std::string& resource_name) const;
    Resource* find_resource_by_name(const std::string& resource_name);

    void ensure_loaded_for_operation(const char* message) const;
    void ensure_archive_stream();

    std::vector<LocalizedString> locstrings_;
    std::vector<Resource> resources_;
    std::vector<std::filesystem::path> new_files_;
    std::vector<FileIdentity> new_file_identities_;
    std::vector<std::string> new_names_;
    std::vector<Resource> new_resource_metadata_;
    bool new_lists_allocated_ = true;


    Header header_;
    bool has_header_ = false;
    std::filesystem::path filename_;
    std::filesystem::path temp_folder_path_;
    std::filesystem::path temp_folder_;
    std::ifstream archive_stream_;
    // archive save frees f_file before assigning the replacement
    //  If Create then raises, f_file still contains the
    // freed object reference rather than nil. Track that narrow dangling-reference
    // boundary separately from an ordinary nil/closed stream.
    bool archive_stream_reference_dangling_ = false;
#if defined(_WIN32)
    void* archive_lock_handle_ = nullptr;
#endif
    bool loaded_ = false;
    bool dirty_ = false;
    bool newfile_ = false;
    bool resref32_ = false;
    ArchiveDiskFormat disk_format_ = ArchiveDiskFormat::ErfV1;
    ResourceNameProfile resource_type_profile_ = ResourceNameProfile::KotOR;
};

std::array<char, 4> archive_type_to_header(ArchiveType type);
ArchiveType archive_type_from_extension(const std::filesystem::path& filename);
std::string archive_type_to_string(ArchiveType type);

} // namespace neoerf
