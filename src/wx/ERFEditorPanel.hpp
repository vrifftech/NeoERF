#pragma once
#include "NeoModulePanel.hpp"
#include "core/ArchiveDocument.hpp"
namespace neoerf::ui {
inline constexpr unsigned kEditorApiVersion=1;
struct MemberOpenTarget {std::string id,label;};
struct MemberOpenHandler {
    std::function<std::vector<MemberOpenTarget>(std::uint16_t)> targets;
    std::function<void(neoshared::ResourceDocument,const std::string&)> open;
};
class ERFEditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path& path)=0;
    virtual bool openGameArchive(const std::filesystem::path& path,std::vector<std::filesystem::path> protectedInputs)=0;
    virtual bool openResource(neoshared::ResourceDocument resource)=0;
    virtual bool newArchive(const std::filesystem::path& path,ArchiveType type)=0;
    virtual bool saveActiveAs(const std::filesystem::path& path)=0;
    virtual bool activateResource(const std::string& identity)=0;
    virtual std::size_t documentCount() const=0;
    virtual ArchiveDocument* activeDocumentModel()=0;
    virtual void refreshActiveDocument()=0;
    virtual void selectResourceProfile(ResourceNameProfile profile)=0;
    virtual void insertResources(const std::vector<std::filesystem::path>& files)=0;
    virtual void setMemberOpenHandler(MemberOpenHandler handler)=0;
    virtual bool openMember(const std::string& name,std::uint16_t type,const std::string& editor={})=0;
};
ERFEditorPanel* createEditorPanel(wxWindow* parent,neomodules::Context context={});
} // namespace neoerf::ui
