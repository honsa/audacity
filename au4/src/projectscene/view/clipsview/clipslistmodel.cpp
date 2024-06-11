/*
* Audacity: A Digital Audio Editor
*/
#include "clipslistmodel.h"

#include "types/projectscenetypes.h"

#include "log.h"

using namespace au::projectscene;
using namespace au::processing;

ClipsListModel::ClipsListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

void ClipsListModel::load()
{
    IF_ASSERT_FAILED(m_trackId >= 0) {
        return;
    }

    dispatcher()->reg(this, "clip-rename", this, &ClipsListModel::onClipRenameAction);

    ProcessingProjectPtr prj = globalContext()->currentProcessingProject();
    if (!prj) {
        return;
    }

    if (m_context) {
        connect(m_context, &TimelineContext::zoomChanged, this, &ClipsListModel::onTimelineContextValuesChanged);
        connect(m_context, &TimelineContext::frameTimeChanged, this, &ClipsListModel::onTimelineContextValuesChanged);
    }

    muse::ValCh<processing::ClipKey> selectedClip = processingInteraction()->selectedClip();
    selectedClip.ch.onReceive(this, [this](const processing::ClipKey& k) {
        onSelectedClip(k);
    });
    onSelectedClip(selectedClip.val);

    beginResetModel();

    m_clipList = prj->clipList(m_trackId);

    m_clipList.onItemChanged(this, [this](const Clip& clip) {
        LOGD() << "onClipChanged, track: " << clip.key.trackId << ", index: " << clip.key.index;
        m_clipList[clip.key.index] = clip;
        QModelIndex idx = this->index(clip.key.index);
        emit dataChanged(idx, idx);
    });

    endResetModel();
}

int ClipsListModel::rowCount(const QModelIndex&) const
{
    return static_cast<int>(m_clipList.size());
}

QHash<int, QByteArray> ClipsListModel::roleNames() const
{
    static QHash<int, QByteArray> roles
    {
        { ClipKeyRole, "clipKey" },
        { ClipTitleRole, "clipTitle" },
        { ClipColorRole, "clipColor" },
        { ClipWidthRole, "clipWidth" },
        { ClipLeftRole, "clipLeft" }
    };
    return roles;
}

QVariant ClipsListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    const au::processing::Clip& clip = m_clipList.at(index.row());
    switch (role) {
    case ClipKeyRole: {
        ClipKey key;
        key.key = clip.key;
        return QVariant::fromValue(key);
    } break;
    case ClipTitleRole:
        return clip.title.toQString();
    case ClipColorRole:
        return clip.color.toQColor();
    case ClipWidthRole: {
        qint64 width = (clip.endTime - clip.startTime) * m_context->zoom();
        return width;
    } break;
    case ClipLeftRole: {
        qint64 left = m_context->timeToPosition(clip.startTime);
        return left;
    } break;
    default:
        break;
    }

    return QVariant();
}

bool ClipsListModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    //LOGD() << "" << index.row() << ", value: " << value << ", role: " << role;
    switch (role) {
    case ClipLeftRole: {
        return changeClipStartTime(index, value);
    } break;
    case ClipTitleRole: {
        return changeClipTitle(index, value);
    } break;
    default:
        break;
    }
    return false;
}

void ClipsListModel::onTimelineContextValuesChanged()
{
    // LOGDA() << "zoom: " << m_context->zoom()
    //         << " frameStartTime: " << m_context->frameStartTime()
    //         << " frameEndTime: " << m_context->frameEndTime();

    for (size_t i = 0; i < m_clipList.size(); ++i) {
        QModelIndex idx = this->index(int(i));
        emit dataChanged(idx, idx, { ClipWidthRole, ClipLeftRole });
    }
}

bool ClipsListModel::changeClipStartTime(const QModelIndex& index, const QVariant& value)
{
    au::processing::Clip& clip = m_clipList[index.row()];
    double sec = m_context->positionToTime(value.toDouble());

    bool ok = processingInteraction()->changeClipStartTime(clip.key, sec);
    return ok;
}

void ClipsListModel::onClipRenameAction(const muse::actions::ActionData& args)
{
    IF_ASSERT_FAILED(args.count() > 0) {
        return;
    }

    processing::ClipKey key = args.arg<processing::ClipKey>(0);

    if (key.trackId != m_trackId) {
        return;
    }

    IF_ASSERT_FAILED(key.index < m_clipList.size()) {
        return;
    }

    emit requestClipTitleEdit(key.index);
}

bool ClipsListModel::changeClipTitle(const QModelIndex& index, const QVariant& value)
{
    au::processing::Clip& clip = m_clipList[index.row()];
    muse::String newTitle = value.toString();
    if (clip.title == newTitle) {
        return false;
    }

    bool ok = processingInteraction()->changeClipTitle(clip.key, newTitle);
    return ok;
}

void ClipsListModel::selectClip(int index)
{
    processingInteraction()->selectClip(processing::ClipKey(m_trackId, index));
}

void ClipsListModel::resetSelectedClip()
{
    processingInteraction()->selectClip(processing::ClipKey());
}

void ClipsListModel::onSelectedClip(const processing::ClipKey& k)
{
    if (m_trackId != k.trackId) {
        setSelectedClipIdx(-1);
    } else {
        setSelectedClipIdx(k.index);
    }
}

QVariant ClipsListModel::trackId() const
{
    return QVariant::fromValue(m_trackId);
}

void ClipsListModel::setTrackId(const QVariant& _newTrackId)
{
    processing::TrackId newTrackId = _newTrackId.toInt();
    if (m_trackId == newTrackId) {
        return;
    }
    m_trackId = newTrackId;
    emit trackIdChanged();
}

TimelineContext* ClipsListModel::timelineContext() const
{
    return m_context;
}

void ClipsListModel::setTimelineContext(TimelineContext* newContext)
{
    if (m_context == newContext) {
        return;
    }
    m_context = newContext;
    emit timelineContextChanged();
}

int ClipsListModel::selectedClipIdx() const
{
    return m_selectedClipIdx;
}

void ClipsListModel::setSelectedClipIdx(int newSelectedClipIdx)
{
    if (m_selectedClipIdx == newSelectedClipIdx) {
        return;
    }
    m_selectedClipIdx = newSelectedClipIdx;
    emit selectedClipIdxChanged();
}
