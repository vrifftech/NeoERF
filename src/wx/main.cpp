#include "erf/Archive.hpp"
#include "erf/ErfPatcher.hpp"
#include "erf/Utils.hpp"
#include "core/Version.hpp"
#include "wx_ui.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoDocumentTabs.hpp"
#include "NeoSettings.hpp"
#include "NeoPatcherExport.hpp"
#include "NeoViewState.hpp"
#include "neoerf_icon.xpm"

#include <wx/aui/auibook.h>
#include <wx/clipbrd.h>
#include <wx/dnd.h>
#include <wx/fdrepdlg.h>
#include <wx/gauge.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/wx.h>
#include <wx/version.h>

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static_assert(wxui::kPatcherExportUiApiVersion >= 3u,
              "NeoERF requires the exact-INI/Fragment patch-export UI from the current neoshared checkout.");
#if defined(__EMSCRIPTEN__)
static_assert(neobrowser::kBrowserFileApiVersion >= 2u,
              "NeoERF WebAssembly requires the asynchronous browser host-file bridge from the current neoshared checkout.");
#endif

namespace {

constexpr const char* kAppName = "NeoERF";
constexpr const char* kArchiveWildcard =
    "All supported archives (*.erf;*.rim;*.rimp;*.crf;*.mod;*.sav;*.nwm;*.hak)|*.erf;*.rim;*.rimp;*.crf;*.mod;*.sav;*.nwm;*.hak|"
    "ERF/CRF file (*.erf;*.crf)|*.erf;*.crf|"
    "Resource Image file (*.rim)|*.rim|"
    "Patch resource image file (*.rimp)|*.rimp|"
    "Module file (*.mod)|*.mod|"
    "Savegame file (*.sav)|*.sav|"
    "NWN module file (*.nwm)|*.nwm|"
    "Hak file (*.hak)|*.hak|"
    "All files (*.*)|*.*";
constexpr const char* kAllFilesWildcard = "All files (*.*)|*.*";
std::string extensionNoDot(const std::filesystem::path& path) {
    std::string ext = neoerf::extension_string(path);
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    return neoerf::ascii_lower(ext);
}

std::string formatSize(std::uintmax_t bytes) {
    if (bytes == 1) {
        return "1 byte";
    }
    if (bytes < 1024) {
        return std::to_string(bytes) + " bytes";
    }
    std::ostringstream out;
    out.setf(std::ios::fixed, std::ios::floatfield);
    if (bytes < 1024 * 1024) {
        out.precision(1);
        out << (static_cast<double>(bytes) / 1024.0) << " kB";
    } else {
        out.precision(2);
        out << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    }
    return out.str();
}

std::string resourceColumnLabel(std::size_t column) {
    switch (column) {
        case 0: return "Resref";
        case 1: return "Type";
        case 2: return "Size";
        default: return "Column " + std::to_string(column);
    }
}


std::string lowerAsciiLocal(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

struct Table {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

constexpr int kRecentFileBaseId = wxID_HIGHEST + 1000;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

bool rowMatches(const Table& table, const std::vector<std::string>& row, const std::string& term) {
    if (term.empty()) return true;
    const std::string needle = lowerAsciiLocal(term);
    for (const auto& column : table.columns) {
        if (lowerAsciiLocal(column).find(needle) != std::string::npos) return true;
    }
    for (const auto& cell : row) {
        if (lowerAsciiLocal(cell).find(needle) != std::string::npos) return true;
    }
    return false;
}

std::vector<std::string> splitLine(const std::string& line, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = line.find(delimiter, start);
        if (pos == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

Table parseClipboardTable(const std::string& text) {
    Table table;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) return table;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    table.columns = splitLine(line, '\t');
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        table.rows.push_back(splitLine(line, '\t'));
    }
    return table;
}

std::string serializeClipboardTable(const Table& table) {
    std::ostringstream out;
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (i) out << '\t';
        out << table.columns[i];
    }
    out << '\n';
    for (const auto& row : table.rows) {
        for (std::size_t i = 0; i < table.columns.size(); ++i) {
            if (i) out << '\t';
            if (i < row.size()) out << row[i];
        }
        out << '\n';
    }
    return out.str();
}

std::size_t optionalColumn(const Table& table, const std::string& name) {
    const auto want = lowerAsciiLocal(name);
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (lowerAsciiLocal(table.columns[i]) == want) return i;
    }
    return table.columns.size();
}

std::string tableCell(const std::vector<std::string>& row, std::size_t index) {
    return index < row.size() ? row[index] : std::string();
}

std::optional<std::filesystem::path> chooseDirectory(wxWindow* parent, const std::string& title) {
#if defined(__EMSCRIPTEN__)
    (void)title;
    wxMessageBox(
        "Multi-resource extraction to a writable directory is unavailable in the browser build. The Extract command can download one selected existing resource at a time. Use a desktop build for multi-resource or directory-wide extraction.",
        "Extraction Unavailable",
        wxOK | wxICON_INFORMATION,
        parent);
    return std::nullopt;
#else
    wxDirDialog dialog(parent, wxui::toWx(title), wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return std::nullopt;
    }
    return std::filesystem::path(wxui::toStd(dialog.GetPath()));
#endif
}

int showReplaceResourceDialog(wxWindow* parent, const std::string& resourceName, bool darkMode) {
    wxDialog dialog(parent, wxID_ANY, "Replace Resource", wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* message = new wxStaticText(&dialog, wxID_ANY,
                                     wxui::toWx("The resource " + resourceName + " already exists in this archive.\nDo you wish to replace it?"));
    message->Wrap(440);
    root->Add(message, 0, wxEXPAND | wxALL, 14);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);
    auto* yes = new wxButton(&dialog, wxID_YES, "Yes");
    auto* yesAll = new wxButton(&dialog, wxID_APPLY, "Yes to all");
    auto* no = new wxButton(&dialog, wxID_NO, "No");
    auto* cancel = new wxButton(&dialog, wxID_CANCEL, "Cancel");
    buttons->Add(yes, 0, wxRIGHT, 6);
    buttons->Add(yesAll, 0, wxRIGHT, 6);
    buttons->Add(no, 0, wxRIGHT, 6);
    buttons->Add(cancel, 0);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    yes->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_YES); });
    yesAll->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_APPLY); });
    no->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_NO); });
    cancel->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });

    dialog.SetSizerAndFit(root);
    wxui::configureResponsiveWindow(dialog, wxSize(560, 240), wxSize(420, 200));
    wxui::applyTheme(&dialog, darkMode);
    dialog.CentreOnParent();
    wxui::constrainWindowToDisplay(dialog);
    return dialog.ShowModal();
}

struct ResourceRow {
    std::string resref;
    std::string archiveName;
    std::string extension;
    std::uint16_t restype = 0xFFFF;
    std::uintmax_t size = 0;
    bool staged = false;

    std::string displayResRef() const {
        const std::string name = archiveName.empty() ? resref : archiveName;
        return staged ? name + "*" : name;
    }
    std::string filename() const { return archiveName.empty() ? (resref + "." + extension) : archiveName; }
};

class NeoERFFrame;

class FileDropTarget final : public wxFileDropTarget {
public:
    explicit FileDropTarget(NeoERFFrame* frame) : frame_(frame) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override;
private:
    NeoERFFrame* frame_ = nullptr;
};

enum : int {
    ID_New = wxID_HIGHEST + 1,
    ID_Open,
    ID_Save,
    ID_SaveAs,
    ID_ExportArchivePatcher,
    ID_CloseTab,
    ID_CloseOtherTabs,
    ID_NextTab,
    ID_PreviousTab,
    ID_DocumentTabs,
    ID_Quit,
    ID_Add,
    ID_Extract,
    ID_Delete,
    ID_Find,
    ID_SelectAll,
    ID_CopyCells,
    ID_PasteCells,
    ID_Filter,
    ID_ClearFilter,
    ID_FilterColumn,
    ID_ClearColumnFilter,
    ID_ClearAllFilters,
    ID_ResetColumnOrder,
    ID_ResetRowOrder,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_ProfileKotOR,
    ID_ProfileJade,
    ID_ProfileNWN,
    ID_ProfileNWN2,
    ID_ProfileWitcher,
    ID_ProfileDAO,
    ID_ProfileDA2,
    ID_ResourceList
};

class NeoERFFrame final : public wxFrame {
public:
    NeoERFFrame()
        : wxFrame(nullptr, wxID_ANY, wxui::toWx(appTitle()), wxDefaultPosition, wxDefaultSize) {
        setApplicationIcon();
        buildMenus();
        buildLayout();
        bindEvents();
        createDocumentTab(true);
        archive().set_resource_type_profile(profile());
        closeArchive(false);
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        applyResourceProfileMenu();
        SetDropTarget(new FileDropTarget(this));
    }

    void rebuildRecentFilesMenu() {
        if (recentFilesMenu_ != nullptr) {
            neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, kRecentFileBaseId, kClearRecentFilesId);
        }
    }

    void rememberRecentFile(const std::filesystem::path& path) {
        settings_.addRecentFile(path);
        rebuildRecentFilesMenu();
    }

    void onOpenRecent(wxCommandEvent& event) {
        const int index = event.GetId() - kRecentFileBaseId;
        const auto files = settings_.recentFiles();
        if (index < 0 || static_cast<std::size_t>(index) >= files.size()) return;
        if (!std::filesystem::exists(files[static_cast<std::size_t>(index)])) {
            settings_.removeRecentFile(files[static_cast<std::size_t>(index)]);
            rebuildRecentFilesMenu();
            wxui::showMessage(this, "Recent File Missing", "Recent file no longer exists:\n" + files[static_cast<std::size_t>(index)].string());
            return;
        }
        openArchive(files[static_cast<std::size_t>(index)], true);
    }

    void onClearRecentFiles(wxCommandEvent&) {
        settings_.clearRecentFiles();
        rebuildRecentFilesMenu();
    }

    bool openArchive(
        const std::filesystem::path& file,
        bool askBeforeDiscard = true,
        std::optional<neoerf::ResourceNameProfile> requestedProfile = std::nullopt) {
        try {
            (void)askBeforeDiscard;
            ensureDocumentTabForOpen();
            if (requestedProfile) {
                profile() = *requestedProfile;
                applyResourceProfileMenu();
            }
            archive().set_resource_type_profile(profile());
            archive().load(file);
            viewState().resetForNewDocument();
            if (filterText_) filterText_->ChangeValue("");
            profile() = archive().resource_type_profile();
            applyResourceProfileMenu();
            stagedRows().clear();
            viewState().sortColumn = 0;
            viewState().sortAscending = true;
            refreshList();
            setStatus("File loaded.", archive().filename().filename().string(), fileCountText());
            rememberRecentFile(file);
            neogames::resolver().inferFromOpenedPath(file);
            updateUiState();
            updateTitle();
            return true;
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            return false;
        }
    }

    void selectResourceProfile(neoerf::ResourceNameProfile profile) {
        setResourceProfile(profile);
    }

    void insertResources(const std::vector<std::filesystem::path>& files) {
        if (!archive().loaded()) {
            return;
        }
        std::size_t inserted = 0;
        bool yesToAll = false;
        setProgressVisible(true, files.size());
        try {
            for (std::size_t i = 0; i < files.size(); ++i) {
                updateProgress(i + 1);
                const auto& file = files[i];
                if (!std::filesystem::is_regular_file(file)) {
                    wxui::showMessage(this, "Unable to add file", "Unable to add file: " + file.string());
                    continue;
                }

                const std::string ext = extensionNoDot(file);
                std::uint16_t type = 0xFFFFu;
                if (!ext.empty()) {
                    type = neoerf::Resource::string_to_res_type(ext, activeProfile());
                }
                if (type == 0xFFFFu && !archive().filename_based_resources()) {
                    wxui::showMessage(this, "Unsupported Resource", "Unable to add file: " + file.string());
                    continue;
                }

                const std::string leaf = file.filename().string();
                const bool exists = archive().filename_based_resources()
                    ? archive().resource_exists_by_name(leaf, true)
                    : archive().resource_exists(leaf, true);
                if (exists && !yesToAll) {
                    const int answer = showReplaceResourceDialog(this, leaf, darkMode_);
                    if (answer == wxID_CANCEL) {
                        break;
                    }
                    if (answer == wxID_NO) {
                        continue;
                    }
                    if (answer == wxID_APPLY) {
                        yesToAll = true;
                    }
                }

                archive().add_resource(file, true);
                std::string resref = neoerf::resource_stem_from_text(leaf);
                if (!archive().filename_based_resources()) {
                    resref = neoerf::string_to_resref(neoerf::ascii_lower(resref), archive().extended_resrefs());
                    eraseStagedDisplayRow(resref, type);
                    stagedRows().push_back(ResourceRow{resref, {}, extensionForType(type), type, std::filesystem::file_size(file), true});
                } else {
                    eraseStagedDisplayRow(leaf, type);
                    stagedRows().push_back(ResourceRow{resref, leaf, ext, type, std::filesystem::file_size(file), true});
                }
                ++inserted;
            }
            refreshList();
            if (inserted > 0) {
                setStatus(inserted == 1 ? "1 new resource added." : std::to_string(inserted) + " new resources added.",
                          archive().filename().filename().string(), fileCountText());
            }
            updateUiState();
            updateTitle();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
        setProgressVisible(false, 0);
    }

private:

    struct DocumentTab {
        std::unique_ptr<neoerf::ErfArchive> archive = std::make_unique<neoerf::ErfArchive>();
        std::vector<ResourceRow> stagedRows;
        neoview::DocumentViewState viewState;
        neoerf::ResourceNameProfile profile = neoerf::ResourceNameProfile::KotOR;
        std::string untitledName = "Untitled ERF";
        wxWindow* tabPage = nullptr;
    };

    bool hasActiveDocument() const {
        return activeDocumentIndex_ != neotabs::npos && activeDocumentIndex_ < documents_.size();
    }

    DocumentTab& activeDocument() { return documents_.at(activeDocumentIndex_); }
    const DocumentTab& activeDocument() const { return documents_.at(activeDocumentIndex_); }
    neoerf::ErfArchive& archive() { return *activeDocument().archive; }
    const neoerf::ErfArchive& archive() const { return *activeDocument().archive; }
    std::vector<ResourceRow>& stagedRows() { return activeDocument().stagedRows; }
    const std::vector<ResourceRow>& stagedRows() const { return activeDocument().stagedRows; }
    neoview::DocumentViewState& viewState() { return activeDocument().viewState; }
    const neoview::DocumentViewState& viewState() const { return activeDocument().viewState; }
    neoerf::ResourceNameProfile& profile() { return activeDocument().profile; }
    const neoerf::ResourceNameProfile& profile() const { return activeDocument().profile; }

    bool tabDirty(const DocumentTab& tab) const { return tab.archive && tab.archive->dirty(); }

    std::string tabDisplayName(const DocumentTab& tab) const {
        return neotabs::displayNameForPath(tab.archive ? tab.archive->filename() : std::filesystem::path{}, tab.untitledName);
    }

    void updateActiveTabTitle() {
        if (!hasActiveDocument()) return;
        neotabs::setTabLabel(documentTabs_, activeDocument().tabPage, tabDisplayName(activeDocument()), tabDirty(activeDocument()));
    }

    void selectDocumentTab(std::size_t index) {
        if (documentTabs_ == nullptr || index >= documents_.size()) return;
        tabSwitchInProgress_ = true;
        const bool selected = neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        if (!selected) return;
        activeDocumentIndex_ = index;
        applyResourceProfileMenu();
        refreshList();
        updateTitle();
        updateUiState();
    }

    void createDocumentTab(bool select = true) {
        DocumentTab tab;
        tab.archive = std::make_unique<neoerf::ErfArchive>();
        tab.archive->set_resource_type_profile(tab.profile);
        tab.viewState.resetForNewDocument();
        tab.viewState.sortColumn = 0;
        tab.viewState.sortAscending = true;
        const std::size_t previousActiveIndex = activeDocumentIndex_;
        documents_.push_back(std::move(tab));
        const std::size_t index = documents_.size() - 1;

        tabSwitchInProgress_ = true;
        wxWindow* const page = neotabs::addTabPage(
            documentTabs_, tabDisplayName(documents_.back()), tabDirty(documents_.back()), select);
        if (page != nullptr) documents_.back().tabPage = page;
        tabSwitchInProgress_ = false;

        if (page == nullptr) {
            documents_.pop_back();
            activeDocumentIndex_ = previousActiveIndex;
            throw std::runtime_error("Unable to create a document tab.");
        }

        if (select) {
            activeDocumentIndex_ = index;
            tabSwitchInProgress_ = true;
            neotabs::changeSelectionToPage(documentTabs_, page);
            tabSwitchInProgress_ = false;
            refreshList();
            updateTitle();
            updateUiState();
        }
    }

    bool activeTabIsReusableForOpen() const {
        return hasActiveDocument() && documents_.size() == 1 && !tabDirty(activeDocument()) && !archive().loaded();
    }

    void ensureDocumentTabForOpen() {
        if (!hasActiveDocument()) { createDocumentTab(true); return; }
        if (!activeTabIsReusableForOpen()) createDocumentTab(true);
    }

    bool confirmCloseDocumentTab(std::size_t index) {
        if (index >= documents_.size()) return true;
        if (!tabDirty(documents_[index])) return true;
        return wxui::confirm(this, "Close tab", neotabs::closePromptText(tabDisplayName(documents_[index])));
    }

    bool closeDocumentTab(std::size_t index) {
        if (index >= documents_.size() || !confirmCloseDocumentTab(index)) return false;

        wxWindow* const page = documents_[index].tabPage;
        tabSwitchInProgress_ = true;
        const bool deleted = neotabs::deleteTabPage(documentTabs_, page);
        tabSwitchInProgress_ = false;
        if (!deleted) return false;

        documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
        if (documents_.empty()) {
            activeDocumentIndex_ = neotabs::npos;
            createDocumentTab(true);
            return true;
        }

        std::size_t selectedIndex = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (selectedIndex == neotabs::npos) selectedIndex = std::min(index, documents_.size() - 1);
        selectDocumentTab(selectedIndex);
        return true;
    }

    bool confirmCloseAllTabs() {
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (!confirmCloseDocumentTab(i)) return false;
        }
        return true;
    }

    void onDocumentTabChanged(wxAuiNotebookEvent& event) {
        if (tabSwitchInProgress_) { event.Skip(); return; }
        const int selection = event.GetSelection();
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) selectDocumentTab(index);
        event.Skip();
    }

    void onDocumentTabCloseRequested(wxAuiNotebookEvent& event) {
        event.Veto();
        const int selection = event.GetSelection();
        if (selection < 0) return;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) closeDocumentTab(index);
    }

    static std::string appTitle() { return std::string("NeoERF v") + neoerf::kVersion + " (ERF/RIM file editor)"; }

    void setApplicationIcon() {
        wxIconBundle bundle;
#if defined(__WXMSW__)
        wxIcon windowsIcon("neoerf", wxBITMAP_TYPE_ICO_RESOURCE);
        if (windowsIcon.IsOk()) {
            bundle.AddIcon(windowsIcon);
        }
#endif
        wxIcon fallbackIcon(neoerf_icon_xpm);
        if (fallbackIcon.IsOk()) {
            bundle.AddIcon(fallbackIcon);
        }
        if (bundle.GetIconCount() > 0) {
            SetIcons(bundle);
        }
    }

    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_New, "&New\tCtrl+N");
        file->Append(ID_Open, "&Open...\tCtrl+O");
        recentFilesMenu_ = new wxMenu;
        rebuildRecentFilesMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->AppendSeparator();
        file->Append(ID_Save, "&Save\tCtrl+S");
        file->Append(ID_SaveAs, "Save &As...\tCtrl+Shift+S");
        file->AppendSeparator();
        file->Append(ID_CloseTab, "&Close Tab\tCtrl-W");
        file->Append(ID_CloseOtherTabs, "Close &Other Tabs");
        file->Append(ID_NextTab, "Next Tab\tCtrl-Tab");
        file->Append(ID_PreviousTab, "Previous Tab\tCtrl-Shift-Tab");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file, [this](const neogames::SavedGameDirectory& directory) {
                chooseAndOpenArchive(
                    directory.path,
                    neoerf::resource_name_profile_for_game_id(directory.gameId));
            });
        file->AppendSeparator();
        file->Append(ID_Quit, "&Quit\tCtrl+Q");

        auto* exportMenu = new wxMenu;
        exportMenu->Append(ID_ExportArchivePatcher,
                           "Export TSLPatcher/HoloPatcher Instructions...");

        auto* tools = new wxMenu;
        tools->Append(ID_Extract, "&Extract selected...\tCtrl+E");
        tools->Append(ID_Delete, "&Delete selected\tCtrl+D");
        tools->AppendSeparator();
        tools->Append(ID_Add, "&Add resources...\tCtrl+I");
        tools->AppendSeparator();
        tools->Append(ID_Find, "&Find in list...\tCtrl+F");
        tools->Append(ID_Filter, "&Filter/Search term...");
        tools->Append(ID_FilterColumn, "Filter Selected &Column...");
        tools->Append(ID_ClearColumnFilter, "Clear Filter on Selected Column");
        tools->Append(ID_ClearAllFilters, "Clear All Filters");
        tools->AppendSeparator();
        tools->Append(ID_SelectAll, "Select &All\tCtrl+A");

        auto* view = new wxMenu;
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        view->AppendSeparator();
        view->Append(ID_ResetColumnOrder, "Reset Column Order");
        view->Append(ID_ResetRowOrder, "Reset Row Order");
        view->AppendSeparator();
        auto* profileMenu = new wxMenu;
        profileKotORItem_ = profileMenu->AppendRadioItem(ID_ProfileKotOR, "Knights of the Old Republic resource names");
        profileJadeItem_ = profileMenu->AppendRadioItem(ID_ProfileJade, "Jade Empire resource names");
        profileNWNItem_ = profileMenu->AppendRadioItem(ID_ProfileNWN, "Neverwinter Nights resource names");
        profileNWN2Item_ = profileMenu->AppendRadioItem(ID_ProfileNWN2, "Neverwinter Nights 2 resource names");
        profileWitcherItem_ = profileMenu->AppendRadioItem(ID_ProfileWitcher, "The Witcher resource names");
        profileDAOItem_ = profileMenu->AppendRadioItem(ID_ProfileDAO, "Dragon Age: Origins filenames");
        profileDA2Item_ = profileMenu->AppendRadioItem(ID_ProfileDA2, "Dragon Age II filenames/hashes");
        view->AppendSubMenu(profileMenu, "Resource &Profile");

        auto* help = new wxMenu;
        help->Append(wxID_ABOUT, "&About");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(exportMenu, "&Export");
        bar->Append(tools, "&Tools");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        SetMenuBar(bar);
    }

    void buildLayout() {
        wxui::createStatusBar(*this, 3);
        const int widths[] = {-3, -2, -1};
        SetStatusWidths(3, widths);

        panel_ = new wxPanel(this, wxID_ANY);
        auto* root = new wxBoxSizer(wxVERTICAL);

        documentTabs_ = new wxAuiNotebook(panel_, ID_DocumentTabs, wxDefaultPosition, wxDefaultSize,
                                          wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB | wxAUI_NB_SCROLL_BUTTONS);
        root->Add(documentTabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        neotabs::configureDocumentTabStrip(documentTabs_);

        auto* headerBox = new wxStaticBoxSizer(wxVERTICAL, panel_, "Archive");
        auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
        fileRow->Add(new wxStaticText(panel_, wxID_ANY, "File:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filePath_ = new wxTextCtrl(panel_, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        fileRow->Add(filePath_, 1, wxEXPAND | wxRIGHT, FromDIP(6));
        fileRow->Add(new wxButton(panel_, ID_Open, "Open..."), 0, wxRIGHT, FromDIP(4));
        fileRow->Add(new wxButton(panel_, ID_Save, "Save"), 0, wxRIGHT, FromDIP(4));
        fileRow->Add(new wxButton(panel_, ID_SaveAs, "Save As..."), 0);
        headerBox->Add(fileRow, 0, wxEXPAND | wxALL, FromDIP(8));

        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(panel_, wxID_ANY, "Filter:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filterText_ = new wxTextCtrl(panel_, wxID_ANY);
        filterRow->Add(filterText_, 1, wxEXPAND | wxRIGHT, FromDIP(4));
        filterRow->Add(new wxButton(panel_, ID_ClearFilter, "Clear"), 0);
        headerBox->Add(filterRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* body = new wxBoxSizer(wxHORIZONTAL);

        list_ = new wxListCtrl(panel_, ID_ResourceList, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_HRULES | wxLC_VRULES);
        list_->AppendColumn("Resref", wxLIST_FORMAT_LEFT, FromDIP(200));
        list_->AppendColumn("Type", wxLIST_FORMAT_LEFT, FromDIP(60));
        list_->AppendColumn("Size", wxLIST_FORMAT_RIGHT, FromDIP(100));
        list_->SetMinSize(FromDIP(wxSize(260, 220)));

        auto* commandColumn = new wxBoxSizer(wxVERTICAL);
        extractButton_ = new wxButton(panel_, ID_Extract, "E", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        insertButton_ = new wxButton(panel_, ID_Add, "+", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        deleteButton_ = new wxButton(panel_, ID_Delete, "-", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        findButton_ = new wxButton(panel_, ID_Find, "F", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        extractButton_->SetToolTip("Extract selected resources.");
        insertButton_->SetToolTip("Add resources to ERF file.");
        deleteButton_->SetToolTip("Delete selected resources from ERF file.");
        findButton_->SetToolTip("Find resource...");
        commandColumn->Add(extractButton_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        commandColumn->Add(insertButton_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        commandColumn->Add(deleteButton_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        commandColumn->Add(findButton_, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        gauge_ = new wxGauge(panel_, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize, wxGA_VERTICAL | wxGA_SMOOTH);
        gauge_->Hide();
        commandColumn->Add(gauge_, 1, wxEXPAND);

        body->Add(list_, 1, wxEXPAND | wxALL, FromDIP(8));
        body->Add(commandColumn, 0, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        root->Add(headerBox, 0, wxEXPAND | wxALL, FromDIP(8));
        root->Add(body, 1, wxEXPAND);
        panel_->SetSizer(root);

        wxui::configureResponsiveWindow(*this, wxSize(760, 620), wxSize(480, 360));
        settings_.restoreWindowPlacement(*this);

        list_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            layoutColumns();
            event.Skip();
        });
        layoutColumns();
    }

    void layoutColumns() {
        if (list_ == nullptr) {
            return;
        }
        const int width = std::max(240, list_->GetClientSize().GetWidth());
        const int typeWidth = FromDIP(64);
        const int sizeWidth = FromDIP(100);
        list_->SetColumnWidth(1, typeWidth);
        list_->SetColumnWidth(2, sizeWidth);
        list_->SetColumnWidth(0, std::max(120, width - typeWidth - sizeWidth - FromDIP(28)));
    }

    void bindEvents() {
        Bind(wxEVT_MENU, &NeoERFFrame::onNew, this, ID_New);
        Bind(wxEVT_MENU, &NeoERFFrame::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &NeoERFFrame::onOpenRecent, this, kRecentFileBaseId, kRecentFileBaseId + neosettings::kMaxRecentFiles - 1);
        Bind(wxEVT_MENU, &NeoERFFrame::onClearRecentFiles, this, kClearRecentFilesId);
        Bind(wxEVT_MENU, &NeoERFFrame::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &NeoERFFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &NeoERFFrame::onExportArchivePatcher, this, ID_ExportArchivePatcher);
        Bind(wxEVT_MENU, &NeoERFFrame::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &NeoERFFrame::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &NeoERFFrame::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &NeoERFFrame::onPreviousTab, this, ID_PreviousTab);
        Bind(wxEVT_MENU, &NeoERFFrame::onAdd, this, ID_Add);
        Bind(wxEVT_MENU, &NeoERFFrame::onExtract, this, ID_Extract);
        Bind(wxEVT_MENU, &NeoERFFrame::onDelete, this, ID_Delete);
        Bind(wxEVT_MENU, &NeoERFFrame::onFind, this, ID_Find);
        Bind(wxEVT_MENU, &NeoERFFrame::onFilterPrompt, this, ID_Filter);
        Bind(wxEVT_MENU, &NeoERFFrame::onClearFilter, this, ID_ClearFilter);
        Bind(wxEVT_MENU, &NeoERFFrame::onFilterSelectedColumn, this, ID_FilterColumn);
        Bind(wxEVT_MENU, &NeoERFFrame::onClearSelectedColumnFilter, this, ID_ClearColumnFilter);
        Bind(wxEVT_MENU, &NeoERFFrame::onClearAllFilters, this, ID_ClearAllFilters);
        Bind(wxEVT_MENU, &NeoERFFrame::onResetColumnOrder, this, ID_ResetColumnOrder);
        Bind(wxEVT_MENU, &NeoERFFrame::onResetRowOrder, this, ID_ResetRowOrder);
        Bind(wxEVT_MENU, &NeoERFFrame::onCopyCells, this, ID_CopyCells);
        Bind(wxEVT_MENU, &NeoERFFrame::onPasteCells, this, ID_PasteCells);
        Bind(wxEVT_MENU, &NeoERFFrame::onSelectAll, this, ID_SelectAll);
        Bind(wxEVT_MENU, &NeoERFFrame::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoERFFrame::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoERFFrame::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoERFFrame::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setResourceProfile(neoerf::ResourceNameProfile::KotOR); }, ID_ProfileKotOR);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setResourceProfile(neoerf::ResourceNameProfile::JadeEmpire); }, ID_ProfileJade);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setResourceProfile(neoerf::ResourceNameProfile::NeverwinterNights); }, ID_ProfileNWN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setResourceProfile(neoerf::ResourceNameProfile::NeverwinterNights2); }, ID_ProfileNWN2);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setResourceProfile(neoerf::ResourceNameProfile::Witcher); }, ID_ProfileWitcher);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setResourceProfile(neoerf::ResourceNameProfile::DragonAgeOrigins); }, ID_ProfileDAO);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setResourceProfile(neoerf::ResourceNameProfile::DragonAge2); }, ID_ProfileDA2);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About NeoERF", std::string("NeoERF v") + neoerf::kVersion + "\nNative wxWidgets ERF/RIM archive editor\n\nA special thanks to everyone in the KOTOR modding community that has contributed their work, knowledge, and creativity to making tools, mods, and guides over the last 20+ years");
        }, wxID_ABOUT);
        Bind(wxEVT_MENU, &NeoERFFrame::onQuit, this, ID_Quit);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onNew, this, ID_New);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onOpen, this, ID_Open);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onAdd, this, ID_Add);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onExtract, this, ID_Extract);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onDelete, this, ID_Delete);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onFind, this, ID_Find);
        Bind(wxEVT_BUTTON, &NeoERFFrame::onClearFilter, this, ID_ClearFilter);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &NeoERFFrame::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &NeoERFFrame::onDocumentTabCloseRequested, this);
        Bind(wxEVT_CLOSE_WINDOW, &NeoERFFrame::onClose, this);

        list_->Bind(wxEVT_LIST_COL_CLICK, &NeoERFFrame::onColumnClick, this);
        list_->Bind(wxEVT_LIST_COL_RIGHT_CLICK, &NeoERFFrame::onListColumnRightClick, this);
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { updateUiState(); });
        list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { updateUiState(); });
        list_->Bind(wxEVT_LIST_KEY_DOWN, &NeoERFFrame::onListKeyDown, this);
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { wxCommandEvent event(wxEVT_MENU, ID_Extract); onExtract(event); });
        list_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &NeoERFFrame::onListContextMenu, this);

        Bind(wxEVT_FIND, &NeoERFFrame::onFindNext, this);
        Bind(wxEVT_FIND_NEXT, &NeoERFFrame::onFindNext, this);
        Bind(wxEVT_FIND_CLOSE, &NeoERFFrame::onFindClose, this);
        if (filterText_) filterText_->Bind(wxEVT_TEXT, &NeoERFFrame::onFilterText, this);
    }

    bool canDiscardDirty(const std::string& action) {
        if (!archive().loaded() || !archive().dirty()) {
            return true;
        }
        return wxui::confirm(this, "Unsaved Changes", "Are you sure you want to " + action + "? Unsaved changes in the current archive will be lost.");
    }

    void closeArchive(bool updateStatus = true) {
        archive().reset();
        viewState().resetForNewDocument();
        if (filterText_) filterText_->ChangeValue("");
        stagedRows().clear();
        displayRows_.clear();
        list_->DeleteAllItems();
        setProgressVisible(false, 0);
        if (updateStatus) {
            setStatus("No file loaded.", "", "");
        }
        updateTitle();
        updateUiState();
    }

    std::vector<long> selectedRows() const {
        std::vector<long> rows;
        long item = -1;
        for (;;) {
            item = list_->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (item < 0) {
                break;
            }
            rows.push_back(item);
        }
        return rows;
    }

    std::vector<ResourceRow> canonicalResourceRows() const {
        std::vector<ResourceRow> rows;
        if (!archive().loaded()) {
            return rows;
        }
        rows.reserve(archive().count() + stagedRows().size());
        for (const auto& resource : archive().resources()) {
            rows.push_back(ResourceRow{resource.resref, resource.filename, resource.extension(activeProfile()), resource.restype, resource.data_size, false});
        }
        for (auto row : stagedRows()) {
            row.extension = extensionForType(row.restype);
            rows.push_back(std::move(row));
        }
        return rows;
    }

    std::string resourceFilterCell(const ResourceRow& row, std::size_t logicalColumn) const {
        switch (logicalColumn) {
            case 0: return row.displayResRef();
            case 1: return neoerf::ascii_upper(row.extension);
            case 2: return formatSize(row.size);
            default: return {};
        }
    }

    std::vector<std::string> resourceVisibleRow(const ResourceRow& row) const {
        return {resourceFilterCell(row, 0), resourceFilterCell(row, 1), resourceFilterCell(row, 2)};
    }

    bool resourceRowPassesCurrentFilters(const ResourceRow& row) const {
        if (!viewState().filterTerm.empty()) {
            Table table;
            table.columns = {"Name", "ResRef", "Extension", "TypeId", "Size", "Staged"};
            if (!rowMatches(table, resourceRow(row), viewState().filterTerm)) {
                return false;
            }
        }
        return neoview::rowPassesColumnFilters(viewState(), [&](std::size_t logicalColumn) {
            return resourceFilterCell(row, logicalColumn);
        });
    }

    void refreshList() {
        displayRows_ = canonicalResourceRows();
        neoview::removeColumnFiltersOutsideRange(viewState(), 3);
        neoview::ensureIdentityColumns(viewState(), 3);
        if (archive().loaded()) {
            sortRows();
            displayRows_.erase(std::remove_if(displayRows_.begin(), displayRows_.end(), [&](const ResourceRow& row) {
                return !resourceRowPassesCurrentFilters(row);
            }), displayRows_.end());
        }
        neoview::setIdentityRows(viewState(), displayRows_.size());
        populateList();
        updateUiState();
    }

    void updateListColumnLabels() {
        for (std::size_t visualColumn = 0; visualColumn < 3; ++visualColumn) {
            const std::size_t logicalColumn = neoview::logicalColumnForVisual(viewState(), visualColumn);
            std::string label = resourceColumnLabel(logicalColumn);
            if (neoview::findColumnFilter(viewState(), logicalColumn) != nullptr) {
                label += " *";
            }
            wxListItem item;
            item.SetMask(wxLIST_MASK_TEXT);
            item.SetText(wxui::toWx(label));
            list_->SetColumn(static_cast<int>(visualColumn), item);
        }
    }

    void populateList() {
        updateListColumnLabels();
        list_->DeleteAllItems();
        for (std::size_t i = 0; i < displayRows_.size(); ++i) {
            const auto& item = displayRows_[i];
            const long row = list_->InsertItem(static_cast<long>(i), wxui::toWx(resourceFilterCell(item, neoview::logicalColumnForVisual(viewState(), 0))));
            list_->SetItem(row, 1, wxui::toWx(resourceFilterCell(item, neoview::logicalColumnForVisual(viewState(), 1))));
            list_->SetItem(row, 2, wxui::toWx(resourceFilterCell(item, neoview::logicalColumnForVisual(viewState(), 2))));
            list_->SetItemData(row, static_cast<long>(i));
        }
        wxui::applyTheme(list_, darkMode_);
    }

    void sortRows() {
        const int column = viewState().sortColumn;
        const bool ascending = viewState().sortAscending;
        std::stable_sort(displayRows_.begin(), displayRows_.end(), [&](const ResourceRow& a, const ResourceRow& b) {
            int cmp = 0;
            if (column == 0) {
                cmp = a.displayResRef().compare(b.displayResRef());
            } else if (column == 1) {
                cmp = a.extension.compare(b.extension);
            } else {
                cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
            }
            if (cmp == 0) {
                cmp = a.filename().compare(b.filename());
            }
            return ascending ? cmp < 0 : cmp > 0;
        });
    }

    std::string fileCountText() const {
        if (!archive().loaded()) {
            return "";
        }
        return std::to_string(archive().count() + archive().count_new()) + " files.";
    }

    std::string profileName() const {
        switch (profile()) {
            case neoerf::ResourceNameProfile::JadeEmpire: return "Jade Empire";
            case neoerf::ResourceNameProfile::NeverwinterNights: return "Neverwinter Nights";
            case neoerf::ResourceNameProfile::NeverwinterNights2: return "Neverwinter Nights 2";
            case neoerf::ResourceNameProfile::Witcher: return "The Witcher";
            case neoerf::ResourceNameProfile::DragonAgeOrigins: return "Dragon Age: Origins";
            case neoerf::ResourceNameProfile::DragonAge2: return "Dragon Age II";
            case neoerf::ResourceNameProfile::KotOR: break;
        }
        return "Knights of the Old Republic";
    }

    neoerf::ResourceNameProfile activeProfile() const {
        return archive().loaded() ? archive().resource_type_profile() : profile();
    }

    std::string extensionForType(std::uint16_t type) const {
        return neoerf::Resource::res_type_to_string(type, activeProfile());
    }

    void setResourceProfile(neoerf::ResourceNameProfile newProfile) {
        if (archive().loaded() && archive().filename_based_resources()) {
            newProfile = archive().resource_type_profile();
        }
        if (profile() == newProfile) {
            applyResourceProfileMenu();
            return;
        }
        profile() = newProfile;
        archive().set_resource_type_profile(profile());
        applyResourceProfileMenu();
        for (auto& row : stagedRows()) {
            row.extension = extensionForType(row.restype);
        }
        refreshList();
        setStatus("Resource profile: " + profileName() + ".", archive().filename().filename().string(), fileCountText());
    }

    void applyResourceProfileMenu() {
        if (profileKotORItem_ != nullptr) {
            profileKotORItem_->Check(profile() == neoerf::ResourceNameProfile::KotOR);
        }
        if (profileJadeItem_ != nullptr) {
            profileJadeItem_->Check(profile() == neoerf::ResourceNameProfile::JadeEmpire);
        }
        if (profileNWNItem_ != nullptr) {
            profileNWNItem_->Check(profile() == neoerf::ResourceNameProfile::NeverwinterNights);
        }
        if (profileNWN2Item_ != nullptr) {
            profileNWN2Item_->Check(profile() == neoerf::ResourceNameProfile::NeverwinterNights2);
        }
        if (profileWitcherItem_ != nullptr) {
            profileWitcherItem_->Check(profile() == neoerf::ResourceNameProfile::Witcher);
        }
        if (profileDAOItem_ != nullptr) {
            profileDAOItem_->Check(profile() == neoerf::ResourceNameProfile::DragonAgeOrigins);
        }
        if (profileDA2Item_ != nullptr) {
            profileDA2Item_->Check(profile() == neoerf::ResourceNameProfile::DragonAge2);
        }
    }

    void setStatus(const std::string& left, const std::string& middle, const std::string& right) {
        std::string statusLeft = left;
        const std::string summary = neoview::columnFilterSummary(viewState());
        if (!summary.empty()) {
            statusLeft += "; filters: " + summary;
        }
        wxui::setStatusText(*this, wxui::toWx(statusLeft), 0);
        wxui::setStatusText(*this, wxui::toWx(middle), 1);
        wxui::setStatusText(*this, wxui::toWx(right), 2);
    }

    void setProgressVisible(bool visible, std::size_t max) {
        gauge_->SetValue(0);
        gauge_->SetRange(static_cast<int>(std::max<std::size_t>(max, 1)));
        gauge_->Show(visible);
        Layout();
    }

    void updateProgress(std::size_t value) {
        gauge_->SetValue(static_cast<int>(value));
        wxYieldIfNeeded();
    }

    void updateTitle() {
        if (!archive().loaded()) {
            SetTitle(wxui::toWx(appTitle()));
            updateActiveTabTitle();
            return;
        }
        std::string title = appTitle() + " - " + archive().filename().string();
        if (archive().dirty()) {
            title += "*";
        }
        SetTitle(wxui::toWx(title));
        updateActiveTabTitle();
    }

    void enableAction(int id, bool enabled) {
        if (GetMenuBar() != nullptr) {
            GetMenuBar()->Enable(id, enabled);
        }
    }

    void updateUiState() {
        const bool loaded = archive().loaded();
        const bool dirty = loaded && archive().dirty();
        const bool hasSelection = !selectedRows().empty();

        enableAction(ID_Save, dirty);
        enableAction(ID_SaveAs, loaded);
        const bool patcherArchive = loaded &&
            profile() == neoerf::ResourceNameProfile::KotOR &&
            !archive().filename_based_resources() &&
            !archive().extended_resrefs() &&
            (archive().disk_format() == neoerf::ArchiveDiskFormat::ErfV1 ||
             archive().disk_format() == neoerf::ArchiveDiskFormat::RimV1);
        enableAction(ID_ExportArchivePatcher, patcherArchive);
        enableAction(ID_Add, loaded);
        enableAction(ID_CopyCells, loaded && hasSelection);
        enableAction(ID_PasteCells, loaded);
        enableAction(ID_Filter, loaded);
        enableAction(ID_ClearFilter, loaded);
        enableAction(ID_FilterColumn, loaded && !displayRows_.empty());
        enableAction(ID_ClearColumnFilter, loaded && !displayRows_.empty());
        enableAction(ID_ClearAllFilters, loaded && neoview::hasAnyFilter(viewState()));
        enableAction(ID_ResetColumnOrder, loaded);
        enableAction(ID_ResetRowOrder, loaded);
        enableAction(ID_Extract, loaded && hasSelection);
        enableAction(ID_Delete, loaded && hasSelection);
        enableAction(ID_Find, loaded && !displayRows_.empty());
        enableAction(ID_SelectAll, loaded && !displayRows_.empty());
        const int toolsIndex = GetMenuBar() ? GetMenuBar()->FindMenu("&Tools") : wxNOT_FOUND;
        if (toolsIndex != wxNOT_FOUND) {
            GetMenuBar()->EnableTop(static_cast<std::size_t>(toolsIndex), loaded);
        }

        if (filePath_) {
            const std::string path = loaded ? archive().filename().string() : std::string();
            if (wxui::toStd(filePath_->GetValue()) != path) filePath_->ChangeValue(wxui::toWx(path));
        }
        if (filterText_) filterText_->Enable(loaded);
        if (insertButton_) insertButton_->Enable(loaded);
        if (extractButton_) extractButton_->Enable(loaded && hasSelection);
        if (deleteButton_) deleteButton_->Enable(loaded && hasSelection);
        if (findButton_) findButton_->Enable(loaded && !displayRows_.empty());
    }


    std::vector<std::string> resourceRow(const ResourceRow& row) const {
        return {row.filename(), row.resref, row.extension, std::to_string(row.restype), std::to_string(row.size), row.staged ? "yes" : "no"};
    }

std::vector<std::filesystem::path> filesFromManifest(const Table& table) const {
        const auto fileCol = optionalColumn(table, "File");
        const auto nameCol = optionalColumn(table, "Name");
        std::vector<std::filesystem::path> files;
        for (const auto& row : table.rows) {
            std::string file = tableCell(row, fileCol);
            if (file.empty()) file = tableCell(row, nameCol);
            if (!file.empty()) files.emplace_back(file);
        }
        return files;
    }

    void setFilterTerm(std::string term) {
        viewState().filterTerm = std::move(term);
        if (filterText_ && wxui::toStd(filterText_->GetValue()) != viewState().filterTerm) filterText_->ChangeValue(wxui::toWx(viewState().filterTerm));
        refreshList();
    }

    void onFilterText(wxCommandEvent&) { setFilterTerm(filterText_ ? wxui::toStd(filterText_->GetValue()) : std::string()); }
    void onFilterPrompt(wxCommandEvent&) {
        const auto term = wxui::promptText(this, "Filter/Search", "Search term:", viewState().filterTerm);
        if (term) setFilterTerm(*term);
    }

    void clearAllFiltersAndRefresh() {
        neoview::clearAllFilters(viewState());
        if (filterText_) filterText_->ChangeValue("");
        refreshList();
    }

    int selectedVisualColumn() const {
        long selected = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (selected >= 0) {
            return 0;
        }
        return contextVisualColumn_ >= 0 ? contextVisualColumn_ : 0;
    }

    void promptColumnFilterForVisualColumn(int visualColumn) {
        if (!archive().loaded()) {
            throw std::runtime_error("No archive loaded.");
        }
        const std::size_t logicalColumn = neoview::logicalColumnForVisual(viewState(), static_cast<std::size_t>(std::max(0, visualColumn)));
        const auto* existing = neoview::findColumnFilter(viewState(), logicalColumn);
        const std::string prior = existing != nullptr ? existing->term : std::string();
        const auto term = wxui::promptText(this, "Column Filter", "Show rows where " + resourceColumnLabel(logicalColumn) + " contains:", prior);
        if (!term) return;
        if (neoview::trimmedCopy(*term).empty()) {
            neoview::clearColumnFilter(viewState(), logicalColumn);
        } else {
            neoview::setColumnFilter(viewState(), neoview::ColumnFilter{logicalColumn, resourceColumnLabel(logicalColumn), *term, neoview::TextFilterMode::Contains, true});
        }
        refreshList();
    }

    void onClearFilter(wxCommandEvent&) { clearAllFiltersAndRefresh(); }
    void onFilterSelectedColumn(wxCommandEvent&) {
        try { promptColumnFilterForVisualColumn(selectedVisualColumn()); } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }
    void onClearSelectedColumnFilter(wxCommandEvent&) {
        const std::size_t logicalColumn = neoview::logicalColumnForVisual(viewState(), static_cast<std::size_t>(std::max(0, selectedVisualColumn())));
        neoview::clearColumnFilter(viewState(), logicalColumn);
        refreshList();
    }
    void onClearAllFilters(wxCommandEvent&) { clearAllFiltersAndRefresh(); }
    void onResetColumnOrder(wxCommandEvent&) { neoview::setIdentityColumns(viewState(), 3); refreshList(); }
    void onResetRowOrder(wxCommandEvent&) { viewState().sortColumn = 0; viewState().sortAscending = true; refreshList(); }

void onCopyCells(wxCommandEvent&) {
        try {
            Table table;
            table.columns = {"Name", "ResRef", "Extension", "TypeId", "Size", "Staged", "File"};
            for (long row : selectedRows()) {
                if (row < 0 || static_cast<std::size_t>(row) >= displayRows_.size()) continue;
                auto cells = resourceRow(displayRows_[static_cast<std::size_t>(row)]);
                cells.push_back({});
                table.rows.push_back(std::move(cells));
            }
            if (table.rows.empty()) return;
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(serializeClipboardTable(table))));
                wxTheClipboard->Close();
            }
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onPasteCells(wxCommandEvent&) {
        try {
            if (!archive().loaded()) throw std::runtime_error("No archive loaded.");
            if (!wxTheClipboard->Open()) return;
            wxTextDataObject data;
            const bool ok = wxTheClipboard->GetData(data);
            wxTheClipboard->Close();
            if (!ok) return;
            auto files = filesFromManifest(parseClipboardTable(wxui::toStd(data.GetText())));
            if (!files.empty()) insertResources(files);
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void eraseStagedDisplayRow(const std::string& resrefOrName, std::uint16_t restype) {
        stagedRows().erase(std::remove_if(stagedRows().begin(), stagedRows().end(), [&](const ResourceRow& row) {
                              if (archive().filename_based_resources()) {
                                  return neoerf::ascii_lower(row.filename()) == neoerf::ascii_lower(resrefOrName);
                              }
                              return neoerf::ascii_lower(row.resref) == neoerf::ascii_lower(resrefOrName) && row.restype == restype;
                          }),
                          stagedRows().end());
    }

    void onNew(wxCommandEvent&) {
        try {
            const auto file = wxui::chooseSaveFile(this, "Create archive", kArchiveWildcard, "new.erf");
            if (!file) {
                return;
            }
            createDocumentTab(true);
            archive().set_resource_type_profile(profile());
            auto type = neoerf::archive_type_from_extension(*file);
            if (profile() == neoerf::ResourceNameProfile::DragonAgeOrigins) {
                type = neoerf::ArchiveType::ERF_V2;
            } else if (profile() == neoerf::ResourceNameProfile::DragonAge2) {
                type = neoerf::ArchiveType::ERF_V3;
            }
            archive().new_archive(*file, type);
            profile() = archive().resource_type_profile();
            applyResourceProfileMenu();
            stagedRows().clear();
            refreshList();
#if defined(__EMSCRIPTEN__)
            setStatus("New archive initialized. Use Save to download it.", file->filename().string(), fileCountText());
#else
            setStatus("File " + file->string() + " created.", file->filename().string(), fileCountText());
#endif
            updateTitle();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void chooseAndOpenArchive(
        const std::filesystem::path& initialDirectory = {},
        std::optional<neoerf::ResourceNameProfile> requestedProfile = std::nullopt) {
        wxui::requestOpenFile(
            this,
            "Open ERF/RIM archive",
            kArchiveWildcard,
            initialDirectory,
            [this, requestedProfile](std::optional<std::filesystem::path> file) {
                if (!file || IsBeingDeleted()) return;
                openArchive(*file, true, requestedProfile);
            });
    }

    void onOpen(wxCommandEvent&) {
        chooseAndOpenArchive();
    }

    void onSave(wxCommandEvent&) {
        try {
            if (!archive().loaded()) {
                throw std::runtime_error("No archive loaded.");
            }
#if !defined(__EMSCRIPTEN__)
            if (!archive().dirty()) {
                return;
            }
#endif
            setProgressVisible(true, 1);
#if defined(__EMSCRIPTEN__)
            // Passing the current virtual path explicitly also serializes a
            // newly created or unchanged archive before downloading it.
            archive().save(archive().filename());
#else
            archive().save();
#endif
#if defined(__EMSCRIPTEN__)
            if (!wxui::publishBrowserFile(
                    archive().filename(), archive().filename().filename().string())) {
                throw std::runtime_error("The browser could not download the archive.");
            }
#endif
            stagedRows().clear();
            refreshList();
            rememberRecentFile(archive().filename());
            neogames::resolver().inferFromOpenedPath(archive().filename());
#if defined(__EMSCRIPTEN__)
            setStatus("Archive downloaded.", archive().filename().filename().string(), fileCountText());
#else
            setStatus("Changes saved to file " + archive().filename().filename().string() + ".", archive().filename().filename().string(), fileCountText());
#endif
            updateTitle();
            setProgressVisible(false, 0);
        } catch (const std::exception& ex) {
            setProgressVisible(false, 0);
            wxui::showError(this, ex);
        }
    }

    void onSaveAs(wxCommandEvent&) {
        try {
            if (!archive().loaded()) {
                throw std::runtime_error("No archive loaded.");
            }
            const auto file = wxui::chooseSaveFile(
                this,
                "Save archive as",
                kArchiveWildcard,
                archive().filename().filename().string());
            if (!file) {
                return;
            }
            setProgressVisible(true, 1);
            archive().save(*file);
#if defined(__EMSCRIPTEN__)
            if (!wxui::publishBrowserFile(*file, file->filename().string())) {
                throw std::runtime_error("The browser could not download the archive.");
            }
#endif
            stagedRows().clear();
            refreshList();
            rememberRecentFile(archive().filename());
            neogames::resolver().inferFromOpenedPath(archive().filename());
#if defined(__EMSCRIPTEN__)
            setStatus("Archive downloaded as " + file->filename().string() + ".", file->filename().string(), fileCountText());
#else
            setStatus("File saved as " + archive().filename().filename().string() + ".", archive().filename().filename().string(), fileCountText());
#endif
            updateTitle();
            setProgressVisible(false, 0);
        } catch (const std::exception& ex) {
            setProgressVisible(false, 0);
            wxui::showError(this, ex);
        }
    }

    void exportArchivePatcherFromOriginal(const std::filesystem::path& originalPath) {
        try {
            if (!archive().loaded()) {
                throw std::runtime_error("No archive is loaded.");
            }

            std::string defaultTarget = archive().filename().filename().string();
            if (extensionNoDot(archive().filename()) == "mod") {
                defaultTarget = "Modules\\" + defaultTarget;
            }
            const auto targetPath = wxui::promptText(
                this,
                "Target Archive Path",
                "Enter the target archive path relative to the game folder (for example Modules\\foo.mod):",
                defaultTarget);
            if (!targetPath) return;

            neoerf::ErfArchive original;
            original.set_resource_type_profile(neoerf::ResourceNameProfile::KotOR);
            original.load(originalPath);
            auto result = neoerf::diffArchivePatcher(original, archive(), *targetPath);
            neotsl::throwIfUnsupported(result.project);

            if (!result.project.warnings.empty()) {
                std::ostringstream warning;
                warning << "NeoERF will install " << result.installCount() << " new resource(s) and replace "
                        << result.replacementCount() << " complete resource(s).\n\n";
                for (const auto& item : result.project.warnings) warning << "- " << item << "\n";
                warning << "\nContinue?";
                if (!wxui::confirm(this, "Archive Resource Patch Warning", warning.str())) return;
            }

            const auto output = wxui::choosePatcherOutput(this);
            if (!output) return;

            if (!output->writesToIni()) {
                std::vector<std::string> payloadNames;
                payloadNames.reserve(result.changes.size());
                for (const auto& change : result.changes) payloadNames.push_back(change.payloadName);
                wxui::showIniFragmentDialog(
                    this,
                    "Archive Patcher INI Fragment",
                    result.project,
                    payloadNames);
                return;
            }

            const bool mergedExisting = std::filesystem::exists(output->iniPath);
            neoerf::writeArchivePatcherPackageToIni(result, archive(), output->iniPath, false);
            setStatus("Patcher package exported.", archive().filename().filename().string(),
                      std::to_string(result.installCount()) + " added, " +
                          std::to_string(result.replacementCount()) + " replaced");
            wxui::showMessage(
                this,
                "Patcher Package Exported",
                std::string(mergedExisting ? "Merged archive instructions into:\n"
                                           : "Created the installer INI:\n") +
                    neosettings::pathToUtf8(output->iniPath) + "\n\nStaged " +
                    std::to_string(result.changes.size()) +
                    " resource payload(s) beside the selected INI.");
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onExportArchivePatcher(wxCommandEvent&) {
        try {
            if (!archive().loaded()) {
                throw std::runtime_error("No archive is loaded.");
            }
            if (archive().dirty()) {
                throw std::runtime_error(
                    "Save the active archive before exporting patcher instructions so the modified archive on disk matches the active tab.");
            }
            if (profile() != neoerf::ResourceNameProfile::KotOR || archive().filename_based_resources() ||
                archive().extended_resrefs() ||
                (archive().disk_format() != neoerf::ArchiveDiskFormat::ErfV1 &&
                 archive().disk_format() != neoerf::ArchiveDiskFormat::RimV1)) {
                throw std::runtime_error(
                    "NeoERF patcher export supports KotOR/KotOR II ERF, RIM, and MOD archives with 16-byte ResRefs only.");
            }

            wxui::requestOpenFile(
                this,
                "Select the clean original archive",
                kArchiveWildcard,
                [this](std::optional<std::filesystem::path> originalPath) {
                    if (!originalPath || IsBeingDeleted()) return;
                    exportArchivePatcherFromOriginal(*originalPath);
                });
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onAdd(wxCommandEvent&) {
        if (!archive().loaded()) {
            return;
        }
        wxui::requestOpenFiles(
            this,
            "Add resources",
            kAllFilesWildcard,
            [this](std::vector<std::filesystem::path> files) {
                if (files.empty() || IsBeingDeleted()) return;
                insertResources(files);
            });
    }

    void onExtract(wxCommandEvent&) {
        try {
            if (!archive().loaded()) {
                throw std::runtime_error("No archive loaded.");
            }
            const auto rows = selectedRows();
            if (rows.empty()) {
                throw std::runtime_error("Select one or more resources first.");
            }
#if defined(__EMSCRIPTEN__)
            if (rows.size() != 1) {
                throw std::runtime_error(
                    "The browser build can download one extracted resource at a time. Use a desktop build for directory-wide or multi-resource extraction.");
            }
            const long data = list_->GetItemData(rows.front());
            if (data < 0 || static_cast<std::size_t>(data) >= displayRows_.size()) {
                throw std::runtime_error("The selected resource is no longer available.");
            }
            const auto& row = displayRows_[static_cast<std::size_t>(data)];
            if (row.staged) {
                throw std::runtime_error("Save the archive before extracting a newly staged resource.");
            }
            const std::string outputName = neoerf::ascii_lower(row.filename());
            const auto output = wxui::chooseSaveFile(this, "Download extracted resource", kAllFilesWildcard, outputName);
            if (!output) {
                return;
            }
            if (archive().filename_based_resources()) {
                archive().get_resource_by_name(row.filename(), *output);
            } else {
                archive().get_resource(row.resref, row.restype, *output);
            }
            if (!wxui::publishBrowserFile(*output, output->filename().string())) {
                throw std::runtime_error("The browser could not download the extracted resource.");
            }
            setStatus("1 resource downloaded.", archive().filename().filename().string(), fileCountText());
            return;
#endif
            const auto directory = chooseDirectory(this, "Select a folder to extract selected resources to:");
            if (!directory) {
                return;
            }

            std::size_t extracted = 0;
            setProgressVisible(true, rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i) {
                updateProgress(i + 1);
                const long data = list_->GetItemData(rows[i]);
                if (data < 0 || static_cast<std::size_t>(data) >= displayRows_.size()) {
                    continue;
                }
                const auto& row = displayRows_[static_cast<std::size_t>(data)];
                if (row.staged) {
                    continue;
                }
                const auto out = *directory / neoerf::ascii_lower(row.filename());
                if (archive().filename_based_resources()) {
                    if (out.has_parent_path()) {
                        std::filesystem::create_directories(out.parent_path());
                    }
                    archive().get_resource_by_name(row.filename(), out);
                } else {
                    archive().get_resource(row.resref, row.restype, out);
                }
                ++extracted;
            }
            setProgressVisible(false, 0);
            setStatus(extracted == 1 ? "1 resource extracted." : std::to_string(extracted) + " resources extracted.",
                      archive().filename().filename().string(), fileCountText());
        } catch (const std::exception& ex) {
            setProgressVisible(false, 0);
            wxui::showError(this, ex);
        }
    }

    void onDelete(wxCommandEvent&) {
        try {
            if (!archive().loaded()) {
                throw std::runtime_error("No archive loaded.");
            }
            auto rows = selectedRows();
            if (rows.empty()) {
                throw std::runtime_error("Select one or more resources first.");
            }
            if (!wxui::confirm(this, "Delete Resources", "Are you sure you wish to remove the selected resources?")) {
                return;
            }
            std::sort(rows.rbegin(), rows.rend());
            std::size_t deleted = 0;
            setProgressVisible(true, rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i) {
                updateProgress(i + 1);
                const long data = list_->GetItemData(rows[i]);
                if (data < 0 || static_cast<std::size_t>(data) >= displayRows_.size()) {
                    continue;
                }
                const auto row = displayRows_[static_cast<std::size_t>(data)];
                if (archive().filename_based_resources()) {
                    archive().delete_resource_by_name(row.filename());
                    if (row.staged) {
                        eraseStagedDisplayRow(row.filename(), row.restype);
                    }
                } else {
                    archive().delete_resource(row.staged ? row.resref + "*" : row.resref, row.restype);
                    if (row.staged) {
                        eraseStagedDisplayRow(row.resref, row.restype);
                    }
                }
                ++deleted;
            }
            refreshList();
            setProgressVisible(false, 0);
            setStatus(deleted == 1 ? "1 resource deleted." : std::to_string(deleted) + " resources deleted.",
                      archive().filename().filename().string(), fileCountText());
            updateTitle();
        } catch (const std::exception& ex) {
            setProgressVisible(false, 0);
            wxui::showError(this, ex);
        }
    }

    void onFind(wxCommandEvent&) {
        if (!findData_) {
            findData_ = std::make_unique<wxFindReplaceData>(wxFR_DOWN);
        }
        if (findDialog_) {
            findDialog_->Raise();
            return;
        }
        findDialog_ = new wxFindReplaceDialog(this, findData_.get(), "Find in list");
        findDialog_->Show(true);
    }

    void onFindNext(wxFindDialogEvent& event) {
        const std::string needle = wxui::toStd(event.GetFindString());
        if (needle.empty() || displayRows_.empty()) {
            return;
        }
        const bool matchCase = (event.GetFlags() & wxFR_MATCHCASE) != 0;
        const bool wholeWord = (event.GetFlags() & wxFR_WHOLEWORD) != 0;
        const bool down = (event.GetFlags() & wxFR_DOWN) != 0;
        if (findFromSelection(needle, matchCase, wholeWord, down)) {
            list_->SetFocus();
        } else {
            wxui::showMessage(this, "Find", "No resources found matching the specified search criteria.");
        }
    }

    bool findFromSelection(std::string needle, bool matchCase, bool wholeWord, bool down) {
        if (!matchCase) {
            needle = neoerf::ascii_lower(needle);
        }
        long selected = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (selected < 0) {
            selected = down ? -1 : static_cast<long>(displayRows_.size());
        }
        auto matches = [&](const ResourceRow& row) {
            std::string haystack = row.displayResRef();
            if (!matchCase) {
                haystack = neoerf::ascii_lower(haystack);
            }
            return wholeWord ? haystack == needle : haystack.find(needle) != std::string::npos;
        };
        if (down) {
            for (long i = selected + 1; i < static_cast<long>(displayRows_.size()); ++i) {
                if (matches(displayRows_[static_cast<std::size_t>(i)])) {
                    list_->SetItemState(-1, 0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                    wxui::selectRow(*list_, i);
                    return true;
                }
            }
        } else {
            for (long i = selected - 1; i >= 0; --i) {
                if (matches(displayRows_[static_cast<std::size_t>(i)])) {
                    list_->SetItemState(-1, 0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                    wxui::selectRow(*list_, i);
                    return true;
                }
            }
        }
        return false;
    }

    void onFindClose(wxFindDialogEvent&) {
        if (findDialog_) {
            findDialog_->Destroy();
            findDialog_ = nullptr;
        }
    }

    void onSelectAll(wxCommandEvent&) {
        for (long row = 0; row < list_->GetItemCount(); ++row) {
            list_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        updateUiState();
    }

    void onCloseTab(wxCommandEvent&) { closeDocumentTab(activeDocumentIndex_); }

    void onCloseOtherTabs(wxCommandEvent&) {
        if (!hasActiveDocument()) return;
        for (std::size_t i = documents_.size(); i-- > 0;) {
            if (i != activeDocumentIndex_ && !closeDocumentTab(i)) return;
        }
    }

    void onNextTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(true);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onPreviousTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(false);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onQuit(wxCommandEvent&) {
        Close();
    }

    void onClose(wxCloseEvent& event) {
        if (event.CanVeto() && !confirmCloseAllTabs()) {
            event.Veto();
            return;
        }
        settings_.saveWindowPlacement(*this);
        event.Skip();
    }

    void applyDarkMode() {
        if (darkModeItem_ != nullptr) {
            darkModeItem_->Check(darkMode_);
        }
        wxui::applyTheme(this, darkMode_);
        if (list_ != nullptr) {
            wxui::applyListTheme(*list_, darkMode_);
        }
        applyFontScale();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
        layoutColumns();
    }

    void changeFontScaleSteps(int steps) {
        const double next = neoview::steppedFontScale(fontScale_, steps);
        if (neoview::fontScalePercent(next) == neoview::fontScalePercent(fontScale_)) return;
        fontScale_ = next;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void onToggleDarkMode(wxCommandEvent& event) {
        darkMode_ = event.IsChecked();
        wxui::writeDarkMode(kAppName, darkMode_);
        applyDarkMode();
    }

    void onIncreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(1);
    }
    void onDecreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(-1);
    }
    void onResetFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }



    void onColumnClick(wxListEvent& event) {
        const int visualColumn = event.GetColumn();
        const int column = static_cast<int>(neoview::logicalColumnForVisual(viewState(), static_cast<std::size_t>(std::max(0, visualColumn))));
        if (viewState().sortColumn == column) {
            viewState().sortAscending = !viewState().sortAscending;
        } else {
            viewState().sortColumn = column;
            viewState().sortAscending = true;
        }
        refreshList();
    }

    void onListColumnRightClick(wxListEvent& event) {
        contextVisualColumn_ = event.GetColumn();
        wxMenu menu;
        menu.Append(ID_FilterColumn, "Filter This Column...");
        menu.Append(ID_ClearColumnFilter, "Clear Filter on This Column");
        menu.AppendSeparator();
        menu.Append(ID_ClearAllFilters, "Clear All Filters");
        PopupMenu(&menu);
    }

    void onListKeyDown(wxListEvent& event) {
        const int key = event.GetKeyCode();
        if (key == WXK_DELETE || key == WXK_BACK) {
            wxCommandEvent deleteEvent(wxEVT_MENU, ID_Delete);
            onDelete(deleteEvent);
        } else {
            event.Skip();
        }
    }

    void onListContextMenu(wxListEvent&) {
        wxMenu menu;
        menu.Append(ID_Extract, "Extract selected...");
        menu.Append(ID_Delete, "Delete selected");
        menu.AppendSeparator();
        menu.Append(ID_FilterColumn, "Filter Selected Column...");
        menu.Append(ID_ClearAllFilters, "Clear All Filters");
        PopupMenu(&menu);
    }

    neosettings::AppSettings settings_{kAppName};
    wxMenu* recentFilesMenu_ = nullptr;
    wxPanel* panel_ = nullptr;
    wxMenuItem* darkModeItem_ = nullptr;
    wxMenuItem* profileJadeItem_ = nullptr;
    wxMenuItem* profileKotORItem_ = nullptr;
    wxMenuItem* profileNWNItem_ = nullptr;
    wxMenuItem* profileNWN2Item_ = nullptr;
    wxMenuItem* profileWitcherItem_ = nullptr;
    wxMenuItem* profileDAOItem_ = nullptr;
    wxMenuItem* profileDA2Item_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxTextCtrl* filePath_ = nullptr;
    wxTextCtrl* filterText_ = nullptr;
    wxGauge* gauge_ = nullptr;
    wxButton* insertButton_ = nullptr;
    wxButton* extractButton_ = nullptr;
    wxButton* deleteButton_ = nullptr;
    wxButton* findButton_ = nullptr;
    wxFindReplaceDialog* findDialog_ = nullptr;
    std::unique_ptr<wxFindReplaceData> findData_;

    wxAuiNotebook* documentTabs_ = nullptr;
    std::vector<DocumentTab> documents_;
    std::size_t activeDocumentIndex_ = neotabs::npos;
    bool tabSwitchInProgress_ = false;
    std::vector<ResourceRow> displayRows_;
    int contextVisualColumn_ = 0;
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool darkMode_ = false;
};

bool FileDropTarget::OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) {
    if (frame_ == nullptr) {
        return false;
    }
    std::vector<std::filesystem::path> files;
    files.reserve(filenames.size());
    for (const auto& filename : filenames) {
        files.emplace_back(wxui::toStd(filename));
    }
    frame_->insertResources(files);
    return true;
}

class NeoERFApp final : public wxApp {
public:
    bool OnInit() override {
#if wxCHECK_VERSION(3, 3, 0)
        SetAppearance(Appearance::System);
#endif
        auto* frame = new NeoERFFrame;
        frame->Show(true);
        int fileArg = 1;
        if (argc > 3) {
            const std::string opt = wxui::toStd(wxString(argv[1]));
            const std::string value = neoerf::ascii_lower(wxui::toStd(wxString(argv[2])));
            if (opt == "--game" || opt == "--profile") {
                if (value == "jade" || value == "jadeempire" || value == "jade-empire" || value == "je") {
                    frame->selectResourceProfile(neoerf::ResourceNameProfile::JadeEmpire);
                } else if (value == "nwn" || value == "nwn1" || value == "neverwinter" || value == "neverwinter1") {
                    frame->selectResourceProfile(neoerf::ResourceNameProfile::NeverwinterNights);
                } else if (value == "nwn2" || value == "neverwinter2") {
                    frame->selectResourceProfile(neoerf::ResourceNameProfile::NeverwinterNights2);
                } else if (value == "witcher" || value == "witcher1" || value == "tw1") {
                    frame->selectResourceProfile(neoerf::ResourceNameProfile::Witcher);
                } else if (value == "dao" || value == "dragonage" || value == "dragonageorigins" || value == "dragon-age-origins") {
                    frame->selectResourceProfile(neoerf::ResourceNameProfile::DragonAgeOrigins);
                } else if (value == "da2" || value == "dragonage2" || value == "dragonageii" || value == "dragon-age-ii" || value == "daii") {
                    frame->selectResourceProfile(neoerf::ResourceNameProfile::DragonAge2);
                } else {
                    frame->selectResourceProfile(neoerf::ResourceNameProfile::KotOR);
                }
                fileArg = 3;
            }
        }
        if (argc > fileArg) {
            frame->openArchive(std::filesystem::path(wxui::toStd(wxString(argv[fileArg]))), false);
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoERFApp);
