/*
* Audacity: A Digital Audio Editor
*/
#ifndef AU_PROJECTSCENE_PLAYBACKTOOLBARCUSOMISEMODEL_H
#define AU_PROJECTSCENE_PLAYBACKTOOLBARCUSOMISEMODEL_H

#include <QHash>

#include "uicomponents/view/selectableitemlistmodel.h"
#include "async/asyncable.h"

#include "modularity/ioc.h"
#include "ui/iuiconfiguration.h"
#include "ui/iuiactionsregister.h"

class QItemSelectionModel;

#if (defined(_MSCVER) || defined(_MSC_VER))
// unreferenced function with internal linkage has been removed
#pragma warning(disable: 4505)
#endif

namespace au::playback {
class PlaybackToolBarCustomiseItem;
class PlaybackToolBarCustomiseModel : public muse::uicomponents::SelectableItemListModel, public muse::async::Asyncable
{
    Q_OBJECT

    muse::Inject<muse::ui::IUiConfiguration> uiConfiguration;
    muse::Inject<muse::ui::IUiActionsRegister> actionsRegister;

    Q_PROPERTY(QItemSelectionModel * selectionModel READ selectionModel NOTIFY selectionChanged)
    Q_PROPERTY(bool isAddSeparatorAvailable READ isAddSeparatorAvailable NOTIFY isAddSeparatorAvailableChanged)

public:
    explicit PlaybackToolBarCustomiseModel(QObject* parent = nullptr);

    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QItemSelectionModel* selectionModel() const;
    bool isAddSeparatorAvailable() const;

    Q_INVOKABLE void load();
    Q_INVOKABLE void addSeparatorLine();

signals:
    void isAddSeparatorAvailableChanged(bool isAddSeparatorAvailable);

private:
    enum Roles {
        ItemRole = SelectableItemListModel::UserRole + 1
    };

    PlaybackToolBarCustomiseItem* modelIndexToItem(const QModelIndex& index) const;

    void onUpdateOperationsAvailability() override;
    void onRowsMoved() override;
    void onRowsRemoved() override;

    void updateRemovingAvailability();
    void updateAddSeparatorAvailability();
    void setIsAddSeparatorAvailable(bool isAddSeparatorAvailable);

    PlaybackToolBarCustomiseItem* makeItem(const muse::ui::UiAction& action, bool checked);
    PlaybackToolBarCustomiseItem* makeSeparatorItem();

    void saveActions();

    bool m_isAddSeparatorAvailable = false;
};
}

#endif // AU_PROJECTSCENE_PLAYBACKTOOLBARCUSOMISEMODEL_H
