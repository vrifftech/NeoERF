#pragma once

#include "erf/Utils.hpp"
#include <neoshared/erf/Archive.hpp>

namespace neoerf {

using neoshared::erf::ArchiveRangeReader;
using neoshared::erf::ErfError;
using neoshared::erf::ArchiveType;
using neoshared::erf::ResourceNameProfile;
using neoshared::erf::resource_name_profile_for_game_id;
using neoshared::erf::ArchiveDiskFormat;
using neoshared::erf::LocalizedString;
using neoshared::erf::Resource;
using neoshared::erf::ErfArchive;
using neoshared::erf::archive_type_to_header;
using neoshared::erf::archive_type_from_extension;
using neoshared::erf::archive_type_to_string;

} // namespace neoerf
