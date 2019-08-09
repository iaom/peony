#include "file-move-operation.h"
#include "file-node-reporter.h"
#include "file-node.h"

using namespace Peony;

FileMoveOperation::FileMoveOperation(QStringList sourceUris, QString destDirUri, QObject *parent) : FileOperation (parent)
{
    m_source_uris = sourceUris;
    m_dest_dir_uri = destDirUri;
}

FileMoveOperation::~FileMoveOperation()
{
    if (m_reporter)
        delete m_reporter;
}

void FileMoveOperation::progress_callback(goffset current_num_bytes,
                                          goffset total_num_bytes,
                                          FileMoveOperation *p_this)
{
    if (p_this->m_force_use_fallback) {
        Q_EMIT p_this->fallbackMoveProgressCallbacked(p_this->m_current_src_uri,
                                                      p_this->m_current_dest_dir_uri,
                                                      current_num_bytes,
                                                      total_num_bytes);
    } else {
        Q_EMIT p_this->nativeMoveProgressCallbacked(p_this->m_current_src_uri,
                                                    p_this->m_current_dest_dir_uri,
                                                    p_this->m_current_count,
                                                    p_this->m_total_count);
    }
    //format: move srcUri to destDirUri: curent_bytes(count) of total_bytes(count).
}

FileOperation::ResponseType FileMoveOperation::prehandle(GError *err)
{
    if (m_prehandle_hash.contains(err->code))
        return m_prehandle_hash.value(err->code);

    return Other;
}

void FileMoveOperation::move()
{
    if (isCancelled())
        return;

    QList<FileNode*> nodes;
    for (auto srcUri : m_source_uris) {
        //FIXME: ignore the total size when using native move.
        addOne(srcUri, 0);
        auto node = new FileNode(srcUri, nullptr, nullptr);
        nodes<<node;
    }
    operationPrepared();

    auto destDir = wrapGFile(g_file_new_for_uri(m_dest_dir_uri.toUtf8().constData()));
    m_total_count = m_source_uris.count();
    for (auto file : nodes) {
        if (isCancelled())
            return;

        QString srcUri = file->uri();
        m_current_count = nodes.indexOf(file) + 1;
        m_current_src_uri = srcUri;
        m_current_dest_dir_uri = m_dest_dir_uri;

        auto srcFile = wrapGFile(g_file_new_for_uri(srcUri.toUtf8().constData()));
        char *base_name = g_file_get_basename(srcFile.get()->get());
        auto destFile = wrapGFile(g_file_resolve_relative_path(destDir.get()->get(),
                                                               base_name));

        char *dest_uri = g_file_get_uri(destFile.get()->get());
        file->setDestUri(dest_uri);

        g_free(dest_uri);
        g_free(base_name);
        GError *err = nullptr;

retry:
        g_file_move(srcFile.get()->get(),
                    destFile.get()->get(),
                    m_default_copy_flag,
                    getCancellable().get()->get(),
                    GFileProgressCallback(progress_callback),
                    this,
                    &err);

        if (err) {
            if (err->code == G_IO_ERROR_CANCELLED) {
                return;
            }
            auto errWrapper = GErrorWrapper::wrapFrom(err);
            ResponseType handle_type = prehandle(err);
            if (handle_type == Other) {
                qDebug()<<"send error";
                auto responseTypeWrapper = Q_EMIT errored(srcUri, m_dest_dir_uri, errWrapper);
                qDebug()<<"get return";
                handle_type = responseTypeWrapper.value<ResponseType>();
                //block until error has been handled.
            }
            switch (handle_type) {
            case IgnoreOne: {
                file->setState(FileNode::HandledButDoNotDeleteDestFile);
                //skip to next loop.
                break;
            }
            case IgnoreAll: {
                file->setState(FileNode::HandledButDoNotDeleteDestFile);
                m_prehandle_hash.insert(err->code, IgnoreOne);
                break;
            }
            case OverWriteOne: {
                file->setState(FileNode::Handled);
                g_file_move(srcFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag|G_FILE_COPY_OVERWRITE),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                break;
            }
            case OverWriteAll: {
                file->setState(FileNode::Handled);
                g_file_move(srcFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag|G_FILE_COPY_OVERWRITE),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                m_prehandle_hash.insert(err->code, OverWriteOne);
                break;
            }
            case BackupOne: {
                file->setState(FileNode::HandledButDoNotDeleteDestFile);
                g_file_move(srcFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag|G_FILE_COPY_BACKUP),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                break;
            }
            case BackupAll: {
                file->setState(FileNode::HandledButDoNotDeleteDestFile);
                g_file_move(srcFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag|G_FILE_COPY_BACKUP),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                m_prehandle_hash.insert(err->code, BackupOne);
                break;
            }
            case Retry: {
                goto retry;
            }
            case Cancel: {
                cancel();
                break;
            }
            default:
                break;
            }
        } else {
            file->setState(FileNode::Handled);
        }
        //FIXME: ignore the total size when using native move.
        fileMoved(srcUri, 0);
    }
    //native move has not clear operation.
    operationProgressed();

    //rollback if cancelled
    //FIXME: if native move function get into error,
    //such as the target is existed, the rollback might
    //get into error too.
    if (isCancelled()) {
        for (auto file : nodes) {
            if (!file->destUri().isEmpty()) {
                GFileWrapperPtr destFile = wrapGFile(g_file_new_for_uri(file->destUri().toUtf8().constData()));
                GFileWrapperPtr srcFile = wrapGFile(g_file_new_for_uri(file->uri().toUtf8().constData()));
                //try rollbacking
                g_file_move(destFile.get()->get(),
                            srcFile.get()->get(),
                            m_default_copy_flag,
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr);
            }
        }
    }

    //release node
    for (auto file : nodes) {
        delete file;
    }
    nodes.clear();
}

void FileMoveOperation::rollbackNodeRecursively(FileNode *node)
{
    switch (node->state()) {
    case FileNode::Handled: {
        if (node->isFolder()) {
            auto children = node->children();
            for (auto child : *children) {
                rollbackNodeRecursively(child);
            }
            GFile *dest_file = g_file_new_for_uri(node->destUri().toUtf8().constData());
            //FIXME: there's a certain probability of failure to delete the folder without
            //any problem happended. because somehow an empty file will created in the folder.
            //i don't know why, but it is obvious that i have to delete them at first.
            bool is_folder_deleted = g_file_delete(dest_file, nullptr, nullptr);
            if (!is_folder_deleted) {
                FileEnumerator e;
                e.setEnumerateDirectory(node->destUri());
                e.enumerateSync();
                for (auto folder_child : *node->children()) {
                    if (!folder_child->destUri().isEmpty()) {
                        GFile *tmp_file = g_file_new_for_uri(folder_child->destUri().toUtf8().constData());
                        g_file_delete(tmp_file, nullptr, nullptr);
                        g_object_unref(tmp_file);
                    }
                    g_file_delete(dest_file, nullptr, nullptr);
                }
            }
            g_object_unref(dest_file);
        } else {
            GFile *dest_file = g_file_new_for_uri(node->destUri().toUtf8().constData());
            g_file_delete(dest_file, nullptr, nullptr);
            g_object_unref(dest_file);
        }
        rollbacked(node->destUri(), node->uri());
        break;
    }
    case FileNode::Cleared: {
        if (node->isFolder()) {
            GFile *src_file = g_file_new_for_uri(node->uri().toUtf8().constData());
            g_file_make_directory(src_file, nullptr, nullptr);
            g_object_unref(src_file);
            auto children = node->children();
            for (auto child : *children) {
                rollbackNodeRecursively(child);
            }
            //try deleting the dest directory
            GFile *dest_file = g_file_new_for_uri(node->destUri().toUtf8().constData());
            g_file_delete(dest_file, nullptr, nullptr);
            g_object_unref(dest_file);
        } else {
            GFile *dest_file = g_file_new_for_uri(node->destUri().toUtf8().constData());
            GFile *src_file = g_file_new_for_uri(node->uri().toUtf8().constData());
            //"rollback"
            GError *err = nullptr;
            g_file_move(dest_file,
                        src_file,
                        m_default_copy_flag,
                        nullptr,
                        nullptr,
                        nullptr,
                        &err);
            if (err) {
                qDebug()<<node->destUri();
                qDebug()<<node->uri();
                qDebug()<<err->message;
                g_error_free(err);
            }
            g_object_unref(dest_file);
            g_object_unref(src_file);
        }
        rollbacked(node->destUri(), node->uri());
        break;
    }
    default: {
        //make sure all nodes were rollbacked.
        if (node->isFolder()) {
            auto children = node->children();
            for (auto child : *children) {
                rollbackNodeRecursively(child);
            }
        }
        break;
    }
    }
}

void FileMoveOperation::copyRecursively(FileNode *node)
{
    if (isCancelled())
        return;

    QString relativePath = node->getRelativePath();
    //FIXME: the smart pointers' deconstruction spends too much time.
    GFileWrapperPtr destRoot = wrapGFile(g_file_new_for_uri(m_dest_dir_uri.toUtf8().constData()));
    GFileWrapperPtr destFile = wrapGFile(g_file_resolve_relative_path(destRoot.get()->get(),
                                                                      relativePath.toUtf8().constData()));

    char *dest_file_uri = g_file_get_uri(destFile.get()->get());
    node->setDestUri(dest_file_uri);
    g_free(dest_file_uri);
    m_current_src_uri = node->uri();
    GFile *dest_parent = g_file_get_parent(destFile.get()->get());
    char *dest_dir_uri = g_file_get_uri(dest_parent);
    m_current_dest_dir_uri = dest_dir_uri;
    g_free(dest_dir_uri);
    g_object_unref(dest_parent);

fallback_retry:
    if (node->isFolder()) {
        GError *err = nullptr;

        //NOTE: mkdir doesn't have a progress callback.
        Q_EMIT fallbackMoveProgressCallbacked(m_current_src_uri,
                                              m_current_dest_dir_uri,
                                              0,
                                              node->size());
        g_file_make_directory(destFile.get()->get(),
                              getCancellable().get()->get(),
                              &err);
        if (err) {
            if (err->code == G_IO_ERROR_CANCELLED) {
                return;
            }
            auto errWrapperPtr = GErrorWrapper::wrapFrom(err);
            ResponseType handle_type = prehandle(err);
            if (handle_type == Other) {
                qDebug()<<"send error";
                auto typeData = errored(m_current_src_uri, m_current_dest_dir_uri, errWrapperPtr);
                qDebug()<<"get return";
                handle_type = typeData.value<ResponseType>();
            }
            //handle.
            switch (handle_type) {
            case IgnoreOne: {
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                break;
            }
            case IgnoreAll: {
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                m_prehandle_hash.insert(err->code, IgnoreOne);
                break;
            }
            case OverWriteOne: {
                node->setState(FileNode::Handled);
                //make dir has no overwrite
                break;
            }
            case OverWriteAll: {
                node->setState(FileNode::Handled);
                m_prehandle_hash.insert(err->code, OverWriteOne);
                break;
            }
            case BackupOne: {
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                //make dir has no backup
                break;
            }
            case BackupAll: {
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                //make dir has no backup
                m_prehandle_hash.insert(err->code, BackupOne);
                break;
            }
            case Retry: {
                goto fallback_retry;
            }
            case Cancel: {
                cancel();
                break;
            }
            default:
                break;
            }
        } else {
            node->setState(FileNode::Handled);
        }
        //assume that make dir finished anyway
        Q_EMIT fallbackMoveProgressCallbacked(m_current_src_uri,
                                              m_current_dest_dir_uri,
                                              node->size(),
                                              node->size());
        Q_EMIT fileMoved(node->uri(), node->size());
        for (auto child : *(node->children())) {
            copyRecursively(child);
        }
    } else {
        GError *err = nullptr;
        GFileWrapperPtr sourceFile = wrapGFile(g_file_new_for_uri(node->uri().toUtf8().constData()));
        g_file_copy(sourceFile.get()->get(),
                    destFile.get()->get(),
                    m_default_copy_flag,
                    getCancellable().get()->get(),
                    GFileProgressCallback(progress_callback),
                    this,
                    &err);

        if (err) {
            if (err->code == G_IO_ERROR_CANCELLED) {
                return;
            }
            auto errWrapperPtr = GErrorWrapper::wrapFrom(err);
            ResponseType handle_type = prehandle(err);
            if (handle_type == Other) {
                qDebug()<<"send error";
                auto typeData = errored(m_current_src_uri, m_current_dest_dir_uri, errWrapperPtr);
                qDebug()<<"get return";
                handle_type = typeData.value<ResponseType>();
            }
            //handle.
            switch (handle_type) {
            case IgnoreOne: {
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                break;
            }
            case IgnoreAll: {
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                m_prehandle_hash.insert(err->code, IgnoreOne);
                break;
            }
            case OverWriteOne: {
                g_file_copy(sourceFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag | G_FILE_COPY_OVERWRITE),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                node->setState(FileNode::Handled);
                break;
            }
            case OverWriteAll: {
                g_file_copy(sourceFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag | G_FILE_COPY_OVERWRITE),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                node->setState(FileNode::Handled);
                m_prehandle_hash.insert(err->code, OverWriteOne);
                break;
            }
            case BackupOne: {
                g_file_copy(sourceFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag | G_FILE_COPY_BACKUP),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                break;
            }
            case BackupAll: {
                g_file_copy(sourceFile.get()->get(),
                            destFile.get()->get(),
                            GFileCopyFlags(m_default_copy_flag | G_FILE_COPY_BACKUP),
                            getCancellable().get()->get(),
                            GFileProgressCallback(progress_callback),
                            this,
                            nullptr);
                node->setState(FileNode::HandledButDoNotDeleteDestFile);
                m_prehandle_hash.insert(err->code, BackupOne);
                break;
            }
            case Retry: {
                goto fallback_retry;
            }
            case Cancel: {
                cancel();
                break;
            }
            default:
                break;
            }
        } else {
            node->setState(FileNode::Handled);
        }
        Q_EMIT fileMoved(node->uri(), node->size());
    }
    destFile.reset();
    destRoot.reset();
}

void FileMoveOperation::deleteRecursively(FileNode *node)
{
    if (isCancelled())
        return;

    GFile *file = g_file_new_for_uri(node->uri().toUtf8().constData());
    if (node->isFolder()) {
        for (auto child : *(node->children())) {
            deleteRecursively(child);
            g_file_delete(file,
                          getCancellable().get()->get(),
                          nullptr);
            node->setState(FileNode::Cleared);
        }
    } else {
        g_file_delete(file,
                      getCancellable().get()->get(),
                      nullptr);
        node->setState(FileNode::Cleared);
    }
    g_object_unref(file);
    qDebug()<<"deleted";
    srcFileDeleted(node->uri());
}

void FileMoveOperation::moveForceUseFallback()
{
    if (isCancelled())
        return;

    m_reporter = new FileNodeReporter;
    connect(m_reporter, &FileNodeReporter::nodeFound, this, &FileMoveOperation::addOne);

    //FIXME: total size should not compute twice. I should get it from ui-thread.
    goffset *total_size = new goffset(0);

    QList<FileNode*> nodes;
    for (auto uri : m_source_uris) {
        FileNode *node = new FileNode(uri, nullptr, m_reporter);
        node->findChildrenRecursively();
        node->computeTotalSize(total_size);
        nodes<<node;
    }
    operationPrepared();

    m_total_szie = *total_size;
    delete total_size;

    for (auto node : nodes) {
        copyRecursively(node);
    }
    operationProgressed();

    for (auto node : nodes) {
        deleteRecursively(node);
    }

    for (auto node : nodes) {
        qDebug()<<node->uri();
    }

    if (isCancelled())
        Q_EMIT operationStartRollbacked();

    for (auto file : nodes) {
        qDebug()<<file->uri();
        if (isCancelled()) {
            rollbackNodeRecursively(file);
        }
    }

    for (auto node : nodes) {
        delete node;
    }

    nodes.clear();
}

void FileMoveOperation::run()
{
    Q_EMIT operationStarted();
    //should block and wait for other object prepared.
    if (!m_force_use_fallback) {
        move();
    } else {
        moveForceUseFallback();
    }
    qDebug()<<"finished";
    Q_EMIT operationFinished();
}

void FileMoveOperation::cancel()
{
    FileOperation::cancel();
    m_reporter->cancel();
}
