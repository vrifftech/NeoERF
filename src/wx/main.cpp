#include "ERFEditorPanel.hpp"
#include "core/Version.hpp"
#include "neoerf_icon.xpm"
#include "NeoSettings.hpp"
#include <wx/app.h>
#include <wx/iconbndl.h>
namespace {
class NeoERFFrame final : public wxFrame {
public:
    NeoERFFrame():wxFrame(nullptr,wxID_ANY,wxui::toWx(std::string("NeoERF v")+neoerf::kVersion)) {
        wxIconBundle icons;
#if defined(__WXMSW__)
        wxIcon native("neoerf",wxBITMAP_TYPE_ICO_RESOURCE);if(native.IsOk())icons.AddIcon(native);
#endif
        wxIcon fallback(neoerf_icon_xpm);if(fallback.IsOk())icons.AddIcon(fallback);
        SetIcons(icons);
        neomodules::Context context;
        context.titleChanged=[this](const wxString& title){SetTitle(title);};
        context.closeRequested=[this]{Close();};
        panel_=neoerf::ui::createEditorPanel(this,std::move(context));
        SetMenuBar(panel_->takeMenus().release());
        auto* layout=new wxBoxSizer(wxVERTICAL);layout->Add(panel_,1,wxEXPAND);SetSizer(layout);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){if(!neomodules::routeCommand({panel_},event))event.Skip();});
        Bind(wxEVT_MENU_OPEN,[this](wxMenuEvent& event){neomodules::routeMenuOpen({panel_},event);event.Skip();});
        Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& event){
            if(!panel_->canClose()){if(event.CanVeto()){event.Veto();return;}}
            settings_.saveWindowPlacement(*this);event.Skip();
        });
        wxui::configureResponsiveWindow(*this,wxSize(1050,720),wxSize(620,420));
        settings_.restoreWindowPlacement(*this);
    }
    ~NeoERFFrame() override {DestroyChildren();}
    void selectProfile(neoerf::ResourceNameProfile profile){panel_->selectResourceProfile(profile);}
    void openStartup(const std::filesystem::path& path){try{panel_->openFile(path);}catch(const std::exception& ex){wxui::showError(this,ex);}}
private:
    neoerf::ui::ERFEditorPanel* panel_{};
    neosettings::AppSettings settings_{"NeoERF"};
};
class NeoERFApp final:public wxApp {
public:bool OnInit()override {
    #if wxCHECK_VERSION(3, 3, 0)
    SetAppearance(Appearance::System);
#endif
    SetAppName("NeoERF");SetVendorName("Neo Tools");wxInitAllImageHandlers();
    auto* frame=new NeoERFFrame;frame->Show();
    int first=1;
    if(argc>3){const auto option=wxui::toStd(wxString(argv[1]));if(option=="--game"||option=="--profile"){
        const auto value=neoerf::ascii_lower(wxui::toStd(wxString(argv[2])));
        auto requested=neoerf::resource_name_profile_for_game_id(value);
        if(value=="jade"||value=="jadeempire"||value=="jade-empire"||value=="je") requested=neoerf::ResourceNameProfile::JadeEmpire;
        else if(value=="nwn"||value=="nwn1"||value=="neverwinter"||value=="neverwinter1") requested=neoerf::ResourceNameProfile::NeverwinterNights;
        else if(value=="nwn2"||value=="neverwinter2") requested=neoerf::ResourceNameProfile::NeverwinterNights2;
        else if(value=="witcher"||value=="witcher1"||value=="tw1") requested=neoerf::ResourceNameProfile::Witcher;
        else if(value=="dao"||value=="dragonage"||value=="dragonageorigins"||value=="dragon-age-origins") requested=neoerf::ResourceNameProfile::DragonAgeOrigins;
        else if(value=="da2"||value=="dragonage2"||value=="dragonageii"||value=="dragon-age-ii"||value=="daii") requested=neoerf::ResourceNameProfile::DragonAge2;
        frame->selectProfile(requested.value_or(neoerf::ResourceNameProfile::KotOR));first=3;
    }}
    for(int i=first;i<argc;++i){const auto path=neosettings::pathFromWx(wxString(argv[i]));frame->CallAfter([frame,path]{frame->openStartup(path);});}
    return true;
}};
}
wxIMPLEMENT_APP(NeoERFApp);
