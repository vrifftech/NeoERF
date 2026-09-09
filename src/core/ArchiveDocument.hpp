#pragma once
#include "erf/Archive.hpp"
#include <neoshared/ResourceDocument.hpp>
#include <algorithm>
#include <memory>
#include <utility>

namespace neoerf {
inline bool isArchiveExtension(std::string ext) {
    ext=ascii_lower(std::move(ext)); if(!ext.empty()&&ext.front()=='.')ext.erase(ext.begin());
    return ext=="erf"||ext=="rim"||ext=="rimp"||ext=="crf"||ext=="mod"||ext=="sav"||ext=="nwm"||ext=="hak";
}
// Document/provenance only. Parsing, compression, staging, and writing remain in
// neoshared::erf. A game-browser origin is copy-on-save, never implicit write-back.
class ArchiveDocument {
public:
    ArchiveDocument():archive_(std::make_unique<ErfArchive>()){}
    ErfArchive& archive() {return *archive_;}
    const ErfArchive& archive() const {return *archive_;}
    const std::string& identity() const {return identity_;}
    const std::string& sourceDescription() const {return description_;}
    const std::vector<std::filesystem::path>& protectedInputs() const {return protected_;}
    bool detached() const {return detached_;}
    bool needsSave() const {return archive_->dirty()||newDocument_;}
    std::filesystem::path path() const {return detached_?std::filesystem::path{}:archive_->filename();}
    std::string displayName() const {return archive_->loaded()?archive_->filename().filename().u8string():"Untitled archive";}
    void open(const std::filesystem::path& path, bool detached=false,
              std::vector<std::filesystem::path> protectedInputs={},
              ResourceNameProfile profile=ResourceNameProfile::KotOR) {
        if(!isArchiveExtension(path.extension().u8string()))throw ErfError("Choose an ERF/RIM-family archive.");
        const auto absolute=std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        auto candidate=std::make_unique<ErfArchive>();candidate->set_resource_type_profile(profile);candidate->load(absolute);
        candidate->release_input_handle(); // no permanent Windows source lock while editing
        archive_=std::move(candidate);identity_="archive:"+absolute.generic_u8string();description_=absolute.u8string();
        detached_=detached;newDocument_=false;protected_=std::move(protectedInputs);
        if(detached_)protected_.push_back(absolute);
    }
    void openResource(neoshared::ResourceDocument input, ResourceNameProfile profile=ResourceNameProfile::KotOR) {
        if(!isArchiveExtension(std::filesystem::path(input.fileName).extension().u8string()))throw ErfError("Not an archive resource.");
        if(input.identity.empty()||std::filesystem::path(input.fileName).filename().u8string()!=input.fileName)
            throw ErfError("Archive snapshot requires a stable identity and a leaf filename.");
        auto bytes=std::make_shared<const std::vector<std::uint8_t>>(std::move(input.bytes));
        auto candidate=std::make_unique<ErfArchive>();candidate->set_resource_type_profile(profile);
        candidate->load_from_reader(input.fileName,bytes->size(),[bytes](std::uint64_t offset,std::size_t length,std::vector<std::uint8_t>& out,std::string& error){
            if(offset>bytes->size()||length>bytes->size()-static_cast<std::size_t>(offset)){error="Archive snapshot read outside payload.";return false;}
            out.assign(bytes->begin()+static_cast<std::ptrdiff_t>(offset),bytes->begin()+static_cast<std::ptrdiff_t>(offset+length));return true;
        });
        archive_=std::move(candidate);identity_=std::move(input.identity);description_=std::move(input.sourceDescription);
        protected_=std::move(input.protectedInputs);detached_=true;newDocument_=false;
    }
    void create(const std::filesystem::path& path,ArchiveType type,ResourceNameProfile profile=ResourceNameProfile::KotOR) {
        if(path.empty()||!isArchiveExtension(path.extension().u8string()))throw ErfError("Choose a supported archive filename.");
        auto candidate=std::make_unique<ErfArchive>();candidate->set_resource_type_profile(profile);
        candidate->new_archive(std::filesystem::absolute(path).lexically_normal(),type);
        archive_=std::move(candidate);identity_="archive:"+archive_->filename().generic_u8string();description_.clear();protected_.clear();detached_=false;newDocument_=true;
    }
    void protectSource(std::vector<std::filesystem::path> inputs) {
        bool sourceIsCurrent=false;
        for(const auto& input:inputs){
            if(neoshared::sameResourcePath(path(),input))sourceIsCurrent=true;
            if(std::find(protected_.begin(),protected_.end(),input)==protected_.end())protected_.push_back(input);
        }
        if(sourceIsCurrent)detached_=true;
    }
    void checkOutput(const std::filesystem::path& output, bool extraction=false) const {
        neoshared::checkResourceOutput(output,protected_);
        neoshared::checkResourceOutput(output,archive_->staged_input_paths());
        if(extraction && archive_->loaded() && !archive_->filename().empty())
            neoshared::checkResourceOutput(output,{archive_->filename()});
    }
    void save(const std::filesystem::path& requested={}) {
        if(!archive_->loaded())throw ErfError("No archive loaded.");
        if(requested.empty()&&detached_)throw ErfError("This game/archive snapshot requires Save As to a separate working archive.");
        const auto output=requested.empty()?archive_->filename():requested;
        if(!isArchiveExtension(output.extension().u8string()))throw ErfError("Choose a supported archive extension.");
        checkOutput(output);
        archive_->save(output); // explicit path also commits a newly created empty archive
        archive_->release_input_handle();detached_=false;newDocument_=false;
    }
    void extract(const std::string& name,const std::filesystem::path& output) {
        checkOutput(output,true);archive_->extract_current_resource(name,output);
    }
    neoshared::ResourceDocument member(const std::string& name,std::uint16_t type) {
        neoshared::ResourceDocument result;
        result.identity=identity_+"/member/"+std::to_string(type)+"/"+ascii_lower(name);
        result.fileName=std::filesystem::path(name).filename().u8string();result.type=type;
        result.sourceDescription=(description_.empty()?archive_->filename().u8string():description_)+" : "+name;
        result.bytes=archive_->read_current_resource(name);result.protectedInputs=protected_;
        if(!archive_->filename().empty()&&!detached_)result.protectedInputs.push_back(archive_->filename());
        const auto staged=archive_->staged_input_paths();result.protectedInputs.insert(result.protectedInputs.end(),staged.begin(),staged.end());
        return result;
    }
private:
    std::unique_ptr<ErfArchive> archive_;
    std::string identity_,description_;
    std::vector<std::filesystem::path> protected_;
    bool detached_=false,newDocument_=false;
};
} // namespace neoerf
