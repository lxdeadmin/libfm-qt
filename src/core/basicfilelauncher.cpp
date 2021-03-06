#include "basicfilelauncher.h"
#include "fileinfojob.h"
#include "mountoperation.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include <unordered_map>
#include <string>

#include <QObject>
#include <QEventLoop>
#include <QDebug>

#include "legacy/fm-app-info.h"

namespace Fm {

BasicFileLauncher::BasicFileLauncher():
    quickExec_{false} {
}

BasicFileLauncher::~BasicFileLauncher() {
}

bool BasicFileLauncher::launchFiles(const FileInfoList& fileInfos, GAppLaunchContext* ctx) {
    std::unordered_map<std::string, FileInfoList> mimeTypeToFiles;
    FileInfoList folderInfos;
    FilePathList pathsToLaunch;
    // classify files according to different mimetypes
    for(auto& fileInfo : fileInfos) {
        // qDebug("path: %s, target: %s", fileInfo->path().toString().get(), fileInfo->target().c_str());
        if(fileInfo->isDir()) {
            folderInfos.emplace_back(fileInfo);
        }
        else if(fileInfo->isMountable()) {
            if(fileInfo->target().empty()) {
                // the mountable is not yet mounted so we have no target URI.
                GErrorPtr err{G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED,
                            QObject::tr("The path is not mounted.")};
                if(!showError(ctx, err, fileInfo->path(), fileInfo)) {
                    // the user fail to handle the error, skip this file.
                    continue;
                }

                // we do not have the target path in the FileInfo object.
                // try to launch our path again to query the new file info later so we can get the mounted target URI.
                pathsToLaunch.emplace_back(fileInfo->path());
            }
            else {
                // we have the target path, launch it later
                pathsToLaunch.emplace_back(FilePath::fromPathStr(fileInfo->target().c_str()));
            }
        }
        else if(fileInfo->isDesktopEntry()) {
            // launch the desktop entry
            launchDesktopEntry(fileInfo, FilePathList{}, ctx);
        }
        else if(fileInfo->isExecutableType()) {
            // directly execute the file
            launchExecutable(fileInfo, ctx);
        }
        else if(fileInfo->isShortcut()) {
            // for shortcuts, launch their targets instead
            auto path = handleShortcut(fileInfo, ctx);
            if(path.isValid()) {
                pathsToLaunch.emplace_back(path);
            }
        }
        else {
            auto& mimeType = fileInfo->mimeType();
            mimeTypeToFiles[mimeType->name()].emplace_back(fileInfo);
        }
    }

    // open folders
    if(!folderInfos.empty()) {
        GErrorPtr err;
        openFolder(ctx, folderInfos, err);
    }

    // open files of different mime-types with their default app
    for(auto& typeFiles : mimeTypeToFiles) {
        auto& mimeType = typeFiles.first;
        auto& files = typeFiles.second;
        GErrorPtr err;
        GAppInfoPtr app{g_app_info_get_default_for_type(mimeType.c_str(), false), false};
        if(!app) {
            app = chooseApp(files, mimeType.c_str(), err);
        }
        if(app) {
            launchWithApp(app.get(), files.paths(), ctx);
        }
    }

    if(!pathsToLaunch.empty()) {
        launchPaths(pathsToLaunch, ctx);
    }

    return true;
}

bool BasicFileLauncher::launchPaths(FilePathList paths, GAppLaunchContext* ctx) {
    // FIXME: blocking with an event loop is not a good design :-(
    QEventLoop eventLoop;

    auto job = new FileInfoJob{paths};
    job->setAutoDelete(false);  // do not automatically delete the job since we want its results later.

    GObjectPtr<GAppLaunchContext> ctxPtr{ctx};
    QObject::connect(job, &FileInfoJob::finished,
            [&eventLoop]() {
        // exit the event loop when the job is done
        eventLoop.exit();
    });
    // run the job in another thread to not block the UI
    job->runAsync();

    // blocking until the job is done with a event loop
    eventLoop.exec();

    // launch the file info
    launchFiles(job->files(), ctx);

    delete job;
    return false;
}

GAppInfoPtr BasicFileLauncher::chooseApp(const FileInfoList& /* fileInfos */, const char* /*mimeType*/, GErrorPtr& /* err */) {
    return GAppInfoPtr{};
}

bool BasicFileLauncher::openFolder(GAppLaunchContext* ctx, const FileInfoList& folderInfos, GErrorPtr& err) {
    auto app = chooseApp(folderInfos, "inode/directory", err);
    if(app) {
        launchWithApp(app.get(), folderInfos.paths(), ctx);
    }
    else {
        showError(ctx, err);
    }
    return false;
}

BasicFileLauncher::ExecAction BasicFileLauncher::askExecFile(const FileInfoPtr & /* file */) {
    return ExecAction::DIRECT_EXEC;
}

bool BasicFileLauncher::showError(GAppLaunchContext* /* ctx */, GErrorPtr& /* err */, const FilePath& /* path */, const FileInfoPtr& /* info */) {
    return false;
}

int BasicFileLauncher::ask(const char* /* msg */, char* const* /* btn_labels */, int default_btn) {
    return default_btn;
}

bool BasicFileLauncher::launchWithApp(GAppInfo* app, const FilePathList& paths, GAppLaunchContext* ctx) {
    GList* uris = nullptr;
    for(auto& path : paths) {
        auto uri = path.uri();
        uris = g_list_prepend(uris, uri.release());
    }
    GErrorPtr err;
    bool ret = bool(g_app_info_launch_uris(app, uris, ctx, &err));
    g_list_foreach(uris, reinterpret_cast<GFunc>(g_free), nullptr);
    g_list_free(uris);
    if(!ret) {
        // FIXME: show error for all files
        showError(ctx, err, paths[0]);
    }
    return ret;
}


bool BasicFileLauncher::launchDesktopEntry(const FileInfoPtr &fileInfo, const FilePathList &paths, GAppLaunchContext* ctx) {
    /* treat desktop entries as executables */
    auto target = fileInfo->target();
    CStrPtr filename;
    const char* desktopEntryName = nullptr;
    FilePathList shortcutTargetPaths;
    if(fileInfo->isExecutableType()) {
        auto act = quickExec_ ? ExecAction::DIRECT_EXEC : askExecFile(fileInfo);
        switch(act) {
        case ExecAction::EXEC_IN_TERMINAL:
        case ExecAction::DIRECT_EXEC: {
            if(fileInfo->isShortcut()) {
                auto path = handleShortcut(fileInfo, ctx);
                if(path.isValid()) {
                    shortcutTargetPaths.emplace_back(path);
                }
            }
            else {
                if(target.empty()) {
                    filename = fileInfo->path().localPath();
                }
                desktopEntryName = !target.empty() ? target.c_str() : filename.get();
            }
            break;
        }
        case ExecAction::OPEN_WITH_DEFAULT_APP:
            return launchWithDefaultApp(fileInfo, ctx);
        case ExecAction::CANCEL:
            return false;
        default:
            return false;
        }
    }
    /* make exception for desktop entries under menu */
    else if(fileInfo->isNative() /* an exception */ ||
            fileInfo->path().hasUriScheme("menu")) {
        if(target.empty()) {
            filename = fileInfo->path().localPath();
        }
        desktopEntryName = !target.empty() ? target.c_str() : filename.get();
    }

    if(desktopEntryName) {
        return launchDesktopEntry(desktopEntryName, paths, ctx);
    }
    if(!shortcutTargetPaths.empty()) {
        launchPaths(shortcutTargetPaths, ctx);
    }
    return false;
}

bool BasicFileLauncher::launchDesktopEntry(const char *desktopEntryName, const FilePathList &paths, GAppLaunchContext *ctx) {
    bool ret = false;
    GAppInfo* app;

    /* Let GDesktopAppInfo try first. */
    if(g_path_is_absolute(desktopEntryName)) {
        app = G_APP_INFO(g_desktop_app_info_new_from_filename(desktopEntryName));
    }
    else {
        app = G_APP_INFO(g_desktop_app_info_new(desktopEntryName));
    }
    /* we handle Type=Link in FmFileInfo so if GIO failed then
       it cannot be launched in fact */

    if(app) {
        return launchWithApp(app, paths, ctx);
    }
    else {
        QString msg = QObject::tr("Invalid desktop entry file: '%1'").arg(desktopEntryName);
        GErrorPtr err{G_IO_ERROR, G_IO_ERROR_FAILED, msg};
        showError(ctx, err);
    }
    return ret;
}

FilePath BasicFileLauncher::handleShortcut(const FileInfoPtr& fileInfo, GAppLaunchContext* ctx) {
    auto target = fileInfo->target();
    auto scheme = CStrPtr{g_uri_parse_scheme(target.c_str())};
    if(scheme) {
        // collect the uri schemes we support
        if(strcmp(scheme.get(), "file") == 0
                || strcmp(scheme.get(), "trash") == 0
                || strcmp(scheme.get(), "network") == 0
                || strcmp(scheme.get(), "computer") == 0) {
            return FilePath::fromUri(fileInfo->target().c_str());
        }
        else {
            // ask gio to launch the default handler for the uri scheme
            GAppInfoPtr app{g_app_info_get_default_for_uri_scheme(scheme.get()), false};
            FilePathList uris{FilePath::fromUri(fileInfo->target().c_str())};
            launchWithApp(app.get(), uris, ctx);
        }
    }
    else {
        // see it as a local path
        return FilePath::fromLocalPath(fileInfo->target().c_str());
    }
    return FilePath();
}

bool BasicFileLauncher::launchExecutable(const FileInfoPtr &fileInfo, GAppLaunchContext* ctx) {
    /* if it's an executable file, directly execute it. */
    auto filename = fileInfo->path().localPath();
    /* FIXME: we need to use eaccess/euidaccess here. */
    if(g_file_test(filename.get(), G_FILE_TEST_IS_EXECUTABLE)) {
        auto act = quickExec_ ? ExecAction::DIRECT_EXEC : askExecFile(fileInfo);
        int flags = G_APP_INFO_CREATE_NONE;
        switch(act) {
        case ExecAction::EXEC_IN_TERMINAL:
            flags |= G_APP_INFO_CREATE_NEEDS_TERMINAL;
        /* Falls through. */
        case ExecAction::DIRECT_EXEC: {
            /* filename may contain spaces. Fix #3143296 */
            CStrPtr quoted{g_shell_quote(filename.get())};
            // FIXME: remove libfm dependency
            GAppInfoPtr app{fm_app_info_create_from_commandline(quoted.get(), nullptr, GAppInfoCreateFlags(flags), nullptr)};
            if(app) {
                CStrPtr run_path{g_path_get_dirname(filename.get())};
                CStrPtr cwd;
                /* bug #3589641: scripts are ran from $HOME.
                   since GIO launcher is kinda ugly - it has
                   no means to set running directory so we
                   do workaround - change directory to it */
                if(run_path && strcmp(run_path.get(), ".")) {
                    cwd = CStrPtr{g_get_current_dir()};
                    if(chdir(run_path.get()) != 0) {
                        cwd.reset();
                        // show errors
                        QString msg = QObject::tr("Cannot set working directory to '%1': %2").arg(run_path.get()).arg(g_strerror(errno));
                        GErrorPtr err{G_IO_ERROR, g_io_error_from_errno(errno), msg};
                        showError(ctx, err);
                    }
                }

                // FIXME: remove libfm dependency
                GErrorPtr err;
                if(!fm_app_info_launch(app.get(), nullptr, ctx, &err)) {
                    showError(ctx, err);
                }
                if(cwd) { /* return back */
                    if(chdir(cwd.get()) != 0) {
                        g_warning("fm_launch_files(): chdir() failed");
                    }
                }
                return true;
            }
            break;
        }
        case ExecAction::OPEN_WITH_DEFAULT_APP:
            return launchWithDefaultApp(fileInfo, ctx);
        case ExecAction::CANCEL:
        default:
            break;
        }
    }
    return false;
}

bool BasicFileLauncher::launchWithDefaultApp(const FileInfoPtr &fileInfo, GAppLaunchContext* ctx) {
    FileInfoList files;
    files.emplace_back(fileInfo);
    GErrorPtr err;
    GAppInfoPtr app{g_app_info_get_default_for_type(fileInfo->mimeType()->name(), false), false};
    if(app) {
        return launchWithApp(app.get(), files.paths(), ctx);
    }
    else {
        showError(ctx, err, fileInfo->path());
    }
    return false;
}

} // namespace Fm
