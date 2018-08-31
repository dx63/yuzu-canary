// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <regex>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QThreadPool>
#include <boost/container/flat_map.hpp>
#include "common/common_paths.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs_real.h"
#include "core/loader/loader.h"
#include "game_list.h"
#include "game_list_p.h"
#include "ui_settings.h"

GameList::SearchField::KeyReleaseEater::KeyReleaseEater(GameList* gamelist) : gamelist{gamelist} {}

// EventFilter in order to process systemkeys while editing the searchfield
bool GameList::SearchField::KeyReleaseEater::eventFilter(QObject* obj, QEvent* event) {
    // If it isn't a KeyRelease event then continue with standard event processing
    if (event->type() != QEvent::KeyRelease)
        return QObject::eventFilter(obj, event);

    QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
    int rowCount = gamelist->tree_view->model()->rowCount();
    QString edit_filter_text = gamelist->search_field->edit_filter->text().toLower();

    // If the searchfield's text hasn't changed special function keys get checked
    // If no function key changes the searchfield's text the filter doesn't need to get reloaded
    if (edit_filter_text == edit_filter_text_old) {
        switch (keyEvent->key()) {
        // Escape: Resets the searchfield
        case Qt::Key_Escape: {
            if (edit_filter_text_old.isEmpty()) {
                return QObject::eventFilter(obj, event);
            } else {
                gamelist->search_field->edit_filter->clear();
                edit_filter_text = "";
            }
            break;
        }
        // Return and Enter
        // If the enter key gets pressed first checks how many and which entry is visible
        // If there is only one result launch this game
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            QStandardItemModel* item_model = new QStandardItemModel(gamelist->tree_view);
            QModelIndex root_index = item_model->invisibleRootItem()->index();
            QStandardItem* child_file;
            QString file_path;
            int resultCount = 0;
            for (int i = 0; i < rowCount; ++i) {
                if (!gamelist->tree_view->isRowHidden(i, root_index)) {
                    ++resultCount;
                    child_file = gamelist->item_model->item(i, 0);
                    file_path = child_file->data(GameListItemPath::FullPathRole).toString();
                }
            }
            if (resultCount == 1) {
                // To avoid loading error dialog loops while confirming them using enter
                // Also users usually want to run a diffrent game after closing one
                gamelist->search_field->edit_filter->setText("");
                edit_filter_text = "";
                emit gamelist->GameChosen(file_path);
            } else {
                return QObject::eventFilter(obj, event);
            }
            break;
        }
        default:
            return QObject::eventFilter(obj, event);
        }
    }
    edit_filter_text_old = edit_filter_text;
    return QObject::eventFilter(obj, event);
}

void GameList::SearchField::setFilterResult(int visible, int total) {
    QString result_of_text = tr("of");
    QString result_text;
    if (total == 1) {
        result_text = tr("result");
    } else {
        result_text = tr("results");
    }
    label_filter_result->setText(
        QString("%1 %2 %3 %4").arg(visible).arg(result_of_text).arg(total).arg(result_text));
}

void GameList::SearchField::clear() {
    edit_filter->setText("");
}

void GameList::SearchField::setFocus() {
    if (edit_filter->isVisible()) {
        edit_filter->setFocus();
    }
}

GameList::SearchField::SearchField(GameList* parent) : QWidget{parent} {
    KeyReleaseEater* keyReleaseEater = new KeyReleaseEater(parent);
    layout_filter = new QHBoxLayout;
    layout_filter->setMargin(8);
    label_filter = new QLabel;
    label_filter->setText(tr("Filter:"));
    edit_filter = new QLineEdit;
    edit_filter->setText("");
    edit_filter->setPlaceholderText(tr("Enter pattern to filter"));
    edit_filter->installEventFilter(keyReleaseEater);
    edit_filter->setClearButtonEnabled(true);
    connect(edit_filter, &QLineEdit::textChanged, parent, &GameList::onTextChanged);
    label_filter_result = new QLabel;
    button_filter_close = new QToolButton(this);
    button_filter_close->setText("X");
    button_filter_close->setCursor(Qt::ArrowCursor);
    button_filter_close->setStyleSheet("QToolButton{ border: none; padding: 0px; color: "
                                       "#000000; font-weight: bold; background: #F0F0F0; }"
                                       "QToolButton:hover{ border: none; padding: 0px; color: "
                                       "#EEEEEE; font-weight: bold; background: #E81123}");
    connect(button_filter_close, &QToolButton::clicked, parent, &GameList::onFilterCloseClicked);
    layout_filter->setSpacing(10);
    layout_filter->addWidget(label_filter);
    layout_filter->addWidget(edit_filter);
    layout_filter->addWidget(label_filter_result);
    layout_filter->addWidget(button_filter_close);
    setLayout(layout_filter);
}

/**
 * Checks if all words separated by spaces are contained in another string
 * This offers a word order insensitive search function
 *
 * @param haystack String that gets checked if it contains all words of the userinput string
 * @param userinput String containing all words getting checked
 * @return true if the haystack contains all words of userinput
 */
static bool ContainsAllWords(const QString& haystack, const QString& userinput) {
    const QStringList userinput_split =
        userinput.split(' ', QString::SplitBehavior::SkipEmptyParts);

    return std::all_of(userinput_split.begin(), userinput_split.end(),
                       [&haystack](const QString& s) { return haystack.contains(s); });
}

// Event in order to filter the gamelist after editing the searchfield
void GameList::onTextChanged(const QString& newText) {
    int rowCount = tree_view->model()->rowCount();
    QString edit_filter_text = newText.toLower();

    QModelIndex root_index = item_model->invisibleRootItem()->index();

    // If the searchfield is empty every item is visible
    // Otherwise the filter gets applied
    if (edit_filter_text.isEmpty()) {
        for (int i = 0; i < rowCount; ++i) {
            tree_view->setRowHidden(i, root_index, false);
        }
        search_field->setFilterResult(rowCount, rowCount);
    } else {
        int result_count = 0;
        for (int i = 0; i < rowCount; ++i) {
            const QStandardItem* child_file = item_model->item(i, 0);
            const QString file_path =
                child_file->data(GameListItemPath::FullPathRole).toString().toLower();
            QString file_name = file_path.mid(file_path.lastIndexOf('/') + 1);
            const QString file_title =
                child_file->data(GameListItemPath::TitleRole).toString().toLower();
            const QString file_programmid =
                child_file->data(GameListItemPath::ProgramIdRole).toString().toLower();

            // Only items which filename in combination with its title contains all words
            // that are in the searchfield will be visible in the gamelist
            // The search is case insensitive because of toLower()
            // I decided not to use Qt::CaseInsensitive in containsAllWords to prevent
            // multiple conversions of edit_filter_text for each game in the gamelist
            if (ContainsAllWords(file_name.append(' ').append(file_title), edit_filter_text) ||
                (file_programmid.count() == 16 && edit_filter_text.contains(file_programmid))) {
                tree_view->setRowHidden(i, root_index, false);
                ++result_count;
            } else {
                tree_view->setRowHidden(i, root_index, true);
            }
            search_field->setFilterResult(result_count, rowCount);
        }
    }
}

void GameList::onFilterCloseClicked() {
    main_window->filterBarSetChecked(false);
}

GameList::GameList(FileSys::VirtualFilesystem vfs, GMainWindow* parent)
    : QWidget{parent}, vfs(std::move(vfs)) {
    watcher = new QFileSystemWatcher(this);
    connect(watcher, &QFileSystemWatcher::directoryChanged, this, &GameList::RefreshGameDirectory);

    this->main_window = parent;
    layout = new QVBoxLayout;
    tree_view = new QTreeView;
    search_field = new SearchField(this);
    item_model = new QStandardItemModel(tree_view);
    tree_view->setModel(item_model);

    tree_view->setAlternatingRowColors(true);
    tree_view->setSelectionMode(QHeaderView::SingleSelection);
    tree_view->setSelectionBehavior(QHeaderView::SelectRows);
    tree_view->setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setSortingEnabled(true);
    tree_view->setEditTriggers(QHeaderView::NoEditTriggers);
    tree_view->setUniformRowHeights(true);
    tree_view->setContextMenuPolicy(Qt::CustomContextMenu);

    item_model->insertColumns(0, COLUMN_COUNT);
    item_model->setHeaderData(COLUMN_NAME, Qt::Horizontal, "Name");
    item_model->setHeaderData(COLUMN_ADD_ONS, Qt::Horizontal, "Add-ons");
    item_model->setHeaderData(COLUMN_FILE_TYPE, Qt::Horizontal, "File type");
    item_model->setHeaderData(COLUMN_SIZE, Qt::Horizontal, "Size");

    connect(tree_view, &QTreeView::activated, this, &GameList::ValidateEntry);
    connect(tree_view, &QTreeView::customContextMenuRequested, this, &GameList::PopupContextMenu);

    // We must register all custom types with the Qt Automoc system so that we are able to use it
    // with signals/slots. In this case, QList falls under the umbrells of custom types.
    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tree_view);
    layout->addWidget(search_field);
    setLayout(layout);
}

GameList::~GameList() {
    emit ShouldCancelWorker();
}

void GameList::setFilterFocus() {
    if (tree_view->model()->rowCount() > 0) {
        search_field->setFocus();
    }
}

void GameList::setFilterVisible(bool visibility) {
    search_field->setVisible(visibility);
}

void GameList::clearFilter() {
    search_field->clear();
}

void GameList::AddEntry(const QList<QStandardItem*>& entry_items) {
    item_model->invisibleRootItem()->appendRow(entry_items);
}

void GameList::ValidateEntry(const QModelIndex& item) {
    // We don't care about the individual QStandardItem that was selected, but its row.
    const int row = item_model->itemFromIndex(item)->row();
    const QStandardItem* child_file = item_model->invisibleRootItem()->child(row, COLUMN_NAME);
    const QString file_path = child_file->data(GameListItemPath::FullPathRole).toString();

    if (file_path.isEmpty())
        return;

    if (!QFileInfo::exists(file_path))
        return;

    const QFileInfo file_info{file_path};
    if (file_info.isDir()) {
        const QDir dir{file_path};
        const QStringList matching_main = dir.entryList(QStringList("main"), QDir::Files);
        if (matching_main.size() == 1) {
            emit GameChosen(dir.path() + DIR_SEP + matching_main[0]);
        }
        return;
    }

    // Users usually want to run a diffrent game after closing one
    search_field->clear();
    emit GameChosen(file_path);
}

void GameList::DonePopulating(QStringList watch_list) {
    // Clear out the old directories to watch for changes and add the new ones
    auto watch_dirs = watcher->directories();
    if (!watch_dirs.isEmpty()) {
        watcher->removePaths(watch_dirs);
    }
    // Workaround: Add the watch paths in chunks to allow the gui to refresh
    // This prevents the UI from stalling when a large number of watch paths are added
    // Also artificially caps the watcher to a certain number of directories
    constexpr int LIMIT_WATCH_DIRECTORIES = 5000;
    constexpr int SLICE_SIZE = 25;
    int len = std::min(watch_list.length(), LIMIT_WATCH_DIRECTORIES);
    for (int i = 0; i < len; i += SLICE_SIZE) {
        watcher->addPaths(watch_list.mid(i, i + SLICE_SIZE));
        QCoreApplication::processEvents();
    }
    tree_view->setEnabled(true);
    int rowCount = tree_view->model()->rowCount();
    search_field->setFilterResult(rowCount, rowCount);
    if (rowCount > 0) {
        search_field->setFocus();
    }
}

void GameList::PopupContextMenu(const QPoint& menu_location) {
    QModelIndex item = tree_view->indexAt(menu_location);
    if (!item.isValid())
        return;

    int row = item_model->itemFromIndex(item)->row();
    QStandardItem* child_file = item_model->invisibleRootItem()->child(row, COLUMN_NAME);
    u64 program_id = child_file->data(GameListItemPath::ProgramIdRole).toULongLong();

    QMenu context_menu;
    QAction* open_save_location = context_menu.addAction(tr("Open Save Data Location"));
    open_save_location->setEnabled(program_id != 0);
    connect(open_save_location, &QAction::triggered,
            [&]() { emit OpenFolderRequested(program_id, GameListOpenTarget::SaveData); });
    context_menu.exec(tree_view->viewport()->mapToGlobal(menu_location));
}

void GameList::PopulateAsync(const QString& dir_path, bool deep_scan) {
    if (!FileUtil::Exists(dir_path.toStdString()) ||
        !FileUtil::IsDirectory(dir_path.toStdString())) {
        LOG_ERROR(Frontend, "Could not find game list folder at {}", dir_path.toLocal8Bit().data());
        search_field->setFilterResult(0, 0);
        return;
    }

    tree_view->setEnabled(false);
    // Delete any rows that might already exist if we're repopulating
    item_model->removeRows(0, item_model->rowCount());

    emit ShouldCancelWorker();

    GameListWorker* worker = new GameListWorker(vfs, dir_path, deep_scan);

    connect(worker, &GameListWorker::EntryReady, this, &GameList::AddEntry, Qt::QueuedConnection);
    connect(worker, &GameListWorker::Finished, this, &GameList::DonePopulating,
            Qt::QueuedConnection);
    // Use DirectConnection here because worker->Cancel() is thread-safe and we want it to cancel
    // without delay.
    connect(this, &GameList::ShouldCancelWorker, worker, &GameListWorker::Cancel,
            Qt::DirectConnection);

    QThreadPool::globalInstance()->start(worker);
    current_worker = std::move(worker);
}

void GameList::SaveInterfaceLayout() {
    UISettings::values.gamelist_header_state = tree_view->header()->saveState();
}

void GameList::LoadInterfaceLayout() {
    auto header = tree_view->header();
    if (!header->restoreState(UISettings::values.gamelist_header_state)) {
        // We are using the name column to display icons and titles
        // so make it as large as possible as default.
        header->resizeSection(COLUMN_NAME, header->width());
    }

    item_model->sort(header->sortIndicatorSection(), header->sortIndicatorOrder());
}

const QStringList GameList::supported_file_extensions = {"nso", "nro", "nca", "xci", "nsp"};

static bool HasSupportedFileExtension(const std::string& file_name) {
    const QFileInfo file = QFileInfo(QString::fromStdString(file_name));
    return GameList::supported_file_extensions.contains(file.suffix(), Qt::CaseInsensitive);
}

static bool IsExtractedNCAMain(const std::string& file_name) {
    return QFileInfo(QString::fromStdString(file_name)).fileName() == "main";
}

static QString FormatGameName(const std::string& physical_name) {
    const QString physical_name_as_qstring = QString::fromStdString(physical_name);
    const QFileInfo file_info(physical_name_as_qstring);

    if (IsExtractedNCAMain(physical_name)) {
        return file_info.dir().path();
    }

    return physical_name_as_qstring;
}

static QString FormatPatchNameVersions(const FileSys::PatchManager& patch_manager,
                                       std::string update_version_override = "",
                                       bool updatable = true) {
    QString out;
    for (const auto& kv : patch_manager.GetPatchVersionNames()) {
        if (!updatable && kv.first == FileSys::PatchType::Update)
            continue;

        if (kv.second == 0) {
            out.append(fmt::format("{}\n", FileSys::FormatPatchTypeName(kv.first)).c_str());
        } else {
            auto version_data = FileSys::FormatTitleVersion(kv.second);
            if (kv.first == FileSys::PatchType::Update && !update_version_override.empty())
                version_data = update_version_override;

            out.append(
                fmt::format("{} ({})\n", FileSys::FormatPatchTypeName(kv.first), version_data)
                    .c_str());
        }
    }

    out.chop(1);
    return out;
}

void GameList::RefreshGameDirectory() {
    if (!UISettings::values.gamedir.isEmpty() && current_worker != nullptr) {
        LOG_INFO(Frontend, "Change detected in the games directory. Reloading game list.");
        search_field->clear();
        PopulateAsync(UISettings::values.gamedir, UISettings::values.gamedir_deepscan);
    }
}

static void GetMetadataFromControlNCA(const FileSys::PatchManager& patch_manager,
                                      const std::shared_ptr<FileSys::NCA>& nca,
                                      std::vector<u8>& icon, std::string& name,
                                      std::string& version) {
    const auto romfs = patch_manager.PatchRomFS(nca->GetRomFS(), nca->GetBaseIVFCOffset(),
                                                FileSys::ContentRecordType::Control);
    if (romfs == nullptr)
        return;

    const auto control_dir = FileSys::ExtractRomFS(romfs);
    if (control_dir == nullptr)
        return;

    const auto nacp_file = control_dir->GetFile("control.nacp");
    if (nacp_file == nullptr)
        return;
    FileSys::NACP nacp(nacp_file);
    name = nacp.GetApplicationName();
    version = nacp.GetVersionString();

    FileSys::VirtualFile icon_file = nullptr;
    for (const auto& language : FileSys::LANGUAGE_NAMES) {
        icon_file = control_dir->GetFile("icon_" + std::string(language) + ".dat");
        if (icon_file != nullptr) {
            icon = icon_file->ReadAllBytes();
            break;
        }
    }
}

void GameListWorker::AddInstalledTitlesToGameList() {
    const auto cache = Service::FileSystem::GetUnionContents();
    const auto installed_games = cache->ListEntriesFilter(FileSys::TitleType::Application,
                                                          FileSys::ContentRecordType::Program);

    for (const auto& game : installed_games) {
        const auto& file = cache->GetEntryUnparsed(game);
        std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(file);
        if (!loader)
            continue;

        std::vector<u8> icon;
        std::string name;
        std::string version;
        u64 program_id = 0;
        loader->ReadProgramId(program_id);

        const FileSys::PatchManager patch{program_id};
        const auto& control = cache->GetEntry(game.title_id, FileSys::ContentRecordType::Control);
        if (control != nullptr)
            GetMetadataFromControlNCA(patch, control, icon, name, version);
        emit EntryReady({
            new GameListItemPath(
                FormatGameName(file->GetFullPath()), icon, QString::fromStdString(name),
                QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType())),
                program_id),
            new GameListItem(FormatPatchNameVersions(patch, version)),
            new GameListItem(
                QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType()))),
            new GameListItemSize(file->GetSize()),
        });
    }

    const auto control_data = cache->ListEntriesFilter(FileSys::TitleType::Application,
                                                       FileSys::ContentRecordType::Control);

    for (const auto& entry : control_data) {
        const auto nca = cache->GetEntry(entry);
        if (nca != nullptr)
            nca_control_map.insert_or_assign(entry.title_id, nca);
    }
}

void GameListWorker::FillControlMap(const std::string& dir_path) {
    const auto nca_control_callback = [this](u64* num_entries_out, const std::string& directory,
                                             const std::string& virtual_name) -> bool {
        std::string physical_name = directory + DIR_SEP + virtual_name;

        if (stop_processing)
            return false; // Breaks the callback loop.

        bool is_dir = FileUtil::IsDirectory(physical_name);
        QFileInfo file_info(physical_name.c_str());
        if (!is_dir && file_info.suffix().toStdString() == "nca") {
            auto nca =
                std::make_shared<FileSys::NCA>(vfs->OpenFile(physical_name, FileSys::Mode::Read));
            if (nca->GetType() == FileSys::NCAContentType::Control)
                nca_control_map.insert_or_assign(nca->GetTitleId(), nca);
        }
        return true;
    };

    FileUtil::ForeachDirectoryEntry(nullptr, dir_path, nca_control_callback);
}

void GameListWorker::AddFstEntriesToGameList(const std::string& dir_path, unsigned int recursion) {
    const auto callback = [this, recursion](u64* num_entries_out, const std::string& directory,
                                            const std::string& virtual_name) -> bool {
        std::string physical_name = directory + DIR_SEP + virtual_name;

        if (stop_processing)
            return false; // Breaks the callback loop.

        bool is_dir = FileUtil::IsDirectory(physical_name);
        if (!is_dir &&
            (HasSupportedFileExtension(physical_name) || IsExtractedNCAMain(physical_name))) {
            std::unique_ptr<Loader::AppLoader> loader =
                Loader::GetLoader(vfs->OpenFile(physical_name, FileSys::Mode::Read));
            if (!loader || ((loader->GetFileType() == Loader::FileType::Unknown ||
                             loader->GetFileType() == Loader::FileType::Error) &&
                            !UISettings::values.show_unknown))
                return true;

            std::vector<u8> icon;
            const auto res1 = loader->ReadIcon(icon);

            u64 program_id = 0;
            const auto res2 = loader->ReadProgramId(program_id);

            std::string name = " ";
            const auto res3 = loader->ReadTitle(name);

            const FileSys::PatchManager patch{program_id};

            std::string version;

            if (res1 != Loader::ResultStatus::Success && res3 != Loader::ResultStatus::Success &&
                res2 == Loader::ResultStatus::Success) {
                // Use from metadata pool.
                if (nca_control_map.find(program_id) != nca_control_map.end()) {
                    const auto nca = nca_control_map[program_id];
                    GetMetadataFromControlNCA(patch, nca, icon, name, version);
                }
            }

            emit EntryReady({
                new GameListItemPath(
                    FormatGameName(physical_name), icon, QString::fromStdString(name),
                    QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType())),
                    program_id),
                new GameListItem(
                    FormatPatchNameVersions(patch, version, loader->IsRomFSUpdatable())),
                new GameListItem(
                    QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType()))),
                new GameListItemSize(FileUtil::GetSize(physical_name)),
            });
        } else if (is_dir && recursion > 0) {
            watch_list.append(QString::fromStdString(physical_name));
            AddFstEntriesToGameList(physical_name, recursion - 1);
        }

        return true;
    };

    FileUtil::ForeachDirectoryEntry(nullptr, dir_path, callback);
}

void GameListWorker::run() {
    stop_processing = false;
    watch_list.append(dir_path);
    FillControlMap(dir_path.toStdString());
    AddInstalledTitlesToGameList();
    AddFstEntriesToGameList(dir_path.toStdString(), deep_scan ? 256 : 0);
    nca_control_map.clear();
    emit Finished(watch_list);
}

void GameListWorker::Cancel() {
    this->disconnect();
    stop_processing = true;
}
