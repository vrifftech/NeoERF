#pragma once

#include <neoshared/erf/Utils.hpp>

// NeoERF historically exposed its archive API from namespace neoerf.  Keep
// that source-compatible surface while the implementation and canonical API
// now live in NeoShared.
namespace neoerf {

using neoshared::erf::kDataSafeIoBufferSize;
using neoshared::erf::FileIdentity;
using neoshared::erf::PathIdentity;
using neoshared::erf::ReplacementTargetState;
using neoshared::erf::ascii_lower;
using neoshared::erf::ascii_upper;
using neoshared::erf::path_to_string;
using neoshared::erf::is_number;
using neoshared::erf::include_trailing_separator;
using neoshared::erf::filename_string;
using neoshared::erf::extension_string;
using neoshared::erf::resource_stem_from_text;
using neoshared::erf::file_exists;
using neoshared::erf::directory_exists;
using neoshared::erf::paths_refer_to_same_existing_file;
using neoshared::erf::paths_refer_to_same_existing_file_or_location;
using neoshared::erf::copy_file_overwrite;
using neoshared::erf::copy_file_overwrite_limited;
using neoshared::erf::copy_file_create_new_limited;
using neoshared::erf::regular_file_size_after_open;
using neoshared::erf::capture_regular_file_identity;
using neoshared::erf::same_regular_file_identity;
using neoshared::erf::ensure_same_regular_file_identity;
using neoshared::erf::capture_path_identity;
using neoshared::erf::same_path_identity;
using neoshared::erf::capture_replacement_target_state;
using neoshared::erf::ensure_replacement_target_unchanged;
using neoshared::erf::remove_file_if_same_identity_noexcept;
using neoshared::erf::remove_tree_if_same_identity_noexcept;
using neoshared::erf::write_regular_file_to_stream_limited;
using neoshared::erf::files_in_folder;
using neoshared::erf::string_to_resref;

} // namespace neoerf
