#include "erf/Archive.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void requireProfile(const char* gameId, neoerf::ResourceNameProfile expected) {
    const auto profile = neoerf::resource_name_profile_for_game_id(gameId);
    require(profile.has_value(), std::string("No resource profile for game ID: ") + gameId);
    require(*profile == expected, std::string("Wrong resource profile for game ID: ") + gameId);
}

std::filesystem::path uniqueTestDirectory() {
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    return std::filesystem::temp_directory_path() /
           ("neoerf-profile-tests-" + std::to_string(stamp));
}

void testDragonAgeFilenameExtensions() {
    using neoerf::Resource;
    using neoerf::ResourceNameProfile;

    require(Resource::string_to_res_type("stg", ResourceNameProfile::DragonAgeOrigins) == 0x9103u,
            "DAO profile does not recognize .stg files");
    require(Resource::string_to_res_type(".plo", ResourceNameProfile::DragonAgeOrigins) == 0x9104u,
            "DAO profile does not recognize .plo files");
    require(Resource::res_type_to_string(0x9103u, ResourceNameProfile::DragonAgeOrigins) == "stg",
            "DAO profile does not display STG resources correctly");
    require(Resource::res_type_to_string(0x9104u, ResourceNameProfile::DragonAgeOrigins) == "plo",
            "DAO profile does not display PLO resources correctly");
}

void testWitcherResourceIds() {
    using neoerf::Resource;
    using neoerf::ResourceNameProfile;

    require(Resource::string_to_res_type("ncs", ResourceNameProfile::Witcher) == 0x07DAu,
            "Witcher profile does not author .ncs with the MOD-compatible type id");
    require(Resource::res_type_to_string(0x07DAu, ResourceNameProfile::Witcher) == "ncs",
            "Witcher profile does not display the standard .ncs type id");
    require(Resource::res_type_to_string(0x0814u, ResourceNameProfile::Witcher) == "ncs",
            "Witcher profile no longer recognizes the alternate .ncs type id");
    require(Resource::string_to_res_type("mmd", ResourceNameProfile::Witcher) == 0x0815u,
            "Witcher profile does not recognize .mmd resources");
}

std::string hexNameHash(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

void testDragonAge2HashedExtensions() {
    using neoerf::ArchiveType;
    using neoerf::ErfArchive;
    using neoerf::Resource;
    using neoerf::ResourceNameProfile;

    struct HashCase {
        const char* extension;
        std::uint32_t typeHash;
        std::uint16_t presentationType;
    };

    const HashCase cases[] = {
        {"bnk", 563723104u, 0x910Cu},
        {"gad", 579662031u, 0x910Du},
        {"vlm", 594026760u, 0x910Eu},
        {"dx11", 1175012015u, 0x910Fu},
        {"gfx", 663550194u, 0x9110u},
        {"rml", 1114824674u, 0x9111u},
        {"anb", 1067198654u, 0x9112u},
        {"cre", 731352151u, 0x9113u},
        {"fxo", 797918166u, 0x9114u},
        {"ldf", 662814705u, 0x9115u},
        {"tnt", 1097649593u, 0x9116u},
    };

    const std::filesystem::path root = uniqueTestDirectory();
    std::filesystem::create_directories(root);
    const std::filesystem::path archivePath = root / "da2-hashed.erf";

    try {
        ErfArchive created;
        created.set_resource_type_profile(ResourceNameProfile::DragonAge2);
        created.new_archive(archivePath, ArchiveType::ERF_V3);

        std::map<std::uint64_t, const HashCase*> expected;
        for (std::size_t i = 0; i < std::size(cases); ++i) {
            const HashCase& testCase = cases[i];
            require(Resource::string_to_res_type(testCase.extension,
                                                 ResourceNameProfile::DragonAge2) ==
                        testCase.presentationType,
                    std::string("DA2 profile does not recognize .") + testCase.extension);
            require(Resource::res_type_to_string(testCase.presentationType,
                                                 ResourceNameProfile::DragonAge2) ==
                        testCase.extension,
                    std::string("DA2 profile does not display .") + testCase.extension);

            const std::uint64_t nameHash = static_cast<std::uint64_t>(i + 1u);
            const std::filesystem::path input =
                root / ("hash_" + hexNameHash(nameHash) + ".#" +
                        std::to_string(testCase.typeHash));
            std::ofstream out(input, std::ios::binary | std::ios::trunc);
            out << "payload-" << testCase.extension;
            out.close();
            require(static_cast<bool>(out), "Could not write a DA2 synthetic test payload");
            created.add_resource(input, false);
            expected.emplace(nameHash, &testCase);
        }
        created.save(archivePath);

        ErfArchive opened;
        opened.set_resource_type_profile(ResourceNameProfile::DragonAge2);
        opened.load(archivePath);
        require(opened.count() == std::size(cases),
                "DA2 hashed-extension archive resource count changed");

        for (const neoerf::Resource& resource : opened.resources()) {
            const auto it = expected.find(resource.v3_name_hash);
            require(it != expected.end(), "DA2 archive changed a stripped resource name hash");
            const HashCase& testCase = *it->second;
            require(resource.v3_type_hash == testCase.typeHash,
                    std::string("DA2 archive changed the type hash for .") +
                        testCase.extension);
            require(resource.filename ==
                        "hash_" + hexNameHash(resource.v3_name_hash) + "." +
                            testCase.extension,
                    std::string("DA2 type hash was not resolved to .") +
                        testCase.extension);
            require(resource.extension(ResourceNameProfile::DragonAge2) ==
                        testCase.extension,
                    std::string("DA2 resource extension() did not return .") +
                        testCase.extension);
        }
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        throw;
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testRequestedProfileSurvivesArchiveLoad() {
    const std::filesystem::path root = uniqueTestDirectory();
    std::filesystem::create_directories(root);

    struct ProfileCase {
        const char* gameId;
        neoerf::ResourceNameProfile profile;
        neoerf::ArchiveType archiveType;
    };

    const std::vector<ProfileCase> profiles = {
        {"kotor", neoerf::ResourceNameProfile::KotOR, neoerf::ArchiveType::ERF},
        {"jade", neoerf::ResourceNameProfile::JadeEmpire, neoerf::ArchiveType::ERF},
        {"nwn", neoerf::ResourceNameProfile::NeverwinterNights, neoerf::ArchiveType::ERF},
        {"nwn2", neoerf::ResourceNameProfile::NeverwinterNights2, neoerf::ArchiveType::ERF},
        {"witcher1", neoerf::ResourceNameProfile::Witcher, neoerf::ArchiveType::ERF},
        {"dao", neoerf::ResourceNameProfile::DragonAgeOrigins, neoerf::ArchiveType::ERF_V2},
        {"da2", neoerf::ResourceNameProfile::DragonAge2, neoerf::ArchiveType::ERF_V3},
    };

    try {
        for (const ProfileCase& testCase : profiles) {
            const std::filesystem::path archivePath =
                root / (std::string(testCase.gameId) + ".erf");

            neoerf::ErfArchive created;
            created.set_resource_type_profile(testCase.profile);
            created.new_archive(archivePath, testCase.archiveType);
            created.save(archivePath);

            neoerf::ErfArchive opened;
            opened.set_resource_type_profile(testCase.profile);
            opened.load(archivePath);
            require(opened.resource_type_profile() == testCase.profile,
                    std::string("Archive loading discarded the requested profile for: ") +
                        testCase.gameId);
        }
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        throw;
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    try {
        requireProfile("kotor", neoerf::ResourceNameProfile::KotOR);
        requireProfile("kotor2", neoerf::ResourceNameProfile::KotOR);
        requireProfile("jade", neoerf::ResourceNameProfile::JadeEmpire);
        requireProfile("nwn", neoerf::ResourceNameProfile::NeverwinterNights);
        requireProfile("nwn2", neoerf::ResourceNameProfile::NeverwinterNights2);
        requireProfile("witcher1", neoerf::ResourceNameProfile::Witcher);
        requireProfile("dao", neoerf::ResourceNameProfile::DragonAgeOrigins);
        requireProfile("da2", neoerf::ResourceNameProfile::DragonAge2);
        require(!neoerf::resource_name_profile_for_game_id("unknown").has_value(),
                "Unknown game ID unexpectedly selected a resource profile");
        testDragonAgeFilenameExtensions();
        testWitcherResourceIds();
        testDragonAge2HashedExtensions();
        testRequestedProfileSurvivesArchiveLoad();
        std::cout << "NeoERF game profile mapping tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NeoERF game profile mapping test failure: " << error.what() << '\n';
        return 1;
    }
}
