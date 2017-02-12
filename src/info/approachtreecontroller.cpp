/*****************************************************************************
* Copyright 2015-2017 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "info/approachtreecontroller.h"

#include "common/unit.h"
#include "gui/mainwindow.h"
#include "common/infoquery.h"
#include "common/approachquery.h"
#include "sql/sqlrecord.h"
#include "ui_mainwindow.h"
#include "gui/actiontextsaver.h"
#include "common/constants.h"
#include "settings/settings.h"
#include "gui/widgetstate.h"
#include "mapgui/mapquery.h"
#include "common/htmlinfobuilder.h"
#include "util/htmlbuilder.h"

#include <QMenu>
#include <QPainter>
#include <QTreeWidget>
#include <QUrlQuery>

enum TreeColumn
{
  DESCRIPTION,
  IDENT,
  ALTITUDE,
  COURSE,
  DISTANCE,
  REMARKS
};

using atools::sql::SqlRecord;
using atools::sql::SqlRecordVector;
using maptypes::MapApproachLeg;
using maptypes::MapApproachLegs;
using maptypes::MapApproachRef;

ApproachTreeController::ApproachTreeController(MainWindow *main)
  : infoQuery(main->getInfoQuery()), approachQuery(main->getApproachQuery()),
    treeWidget(main->getUi()->treeWidgetApproachInfo), mainWindow(main)
{
  infoBuilder = new HtmlInfoBuilder(mainWindow, true);

  treeWidget->headerItem()->setText(DESCRIPTION, tr("Description"));
  treeWidget->headerItem()->setText(IDENT, tr("Ident"));
  treeWidget->headerItem()->setText(ALTITUDE, tr("Altitude"));
  treeWidget->headerItem()->setText(COURSE, tr("Course"));
  treeWidget->headerItem()->setText(DISTANCE, tr("Distance/Time"));
  treeWidget->headerItem()->setText(REMARKS, tr("Remarks"));

  connect(treeWidget, &QTreeWidget::itemSelectionChanged, this, &ApproachTreeController::itemSelectionChanged);
  connect(treeWidget, &QTreeWidget::itemDoubleClicked, this, &ApproachTreeController::itemDoubleClicked);
  connect(treeWidget, &QTreeWidget::itemExpanded, this, &ApproachTreeController::itemExpanded);
  connect(treeWidget, &QTreeWidget::customContextMenuRequested, this, &ApproachTreeController::contextMenu);
  connect(mainWindow->getUi()->textBrowserApproachInfo,
          &QTextBrowser::anchorClicked, this, &ApproachTreeController::anchorClicked);

  currentAirport.id = -1;

  QTreeWidgetItem *root = treeWidget->invisibleRootItem();
  runwayFont = root->font(DESCRIPTION);
  runwayFont.setWeight(QFont::Bold);

  approachFont = root->font(DESCRIPTION);
  approachFont.setWeight(QFont::Bold);

  transitionFont = root->font(DESCRIPTION);
  transitionFont.setWeight(QFont::Bold);

  legFont = root->font(DESCRIPTION);
  // legFont.setPointSizeF(legFont.pointSizeF() * 0.85f);

  missedLegFont = root->font(DESCRIPTION);
  // missedLegFont.setItalic(true);

  invalidLegFont = legFont;
  invalidLegFont.setBold(true);

  Ui::MainWindow *ui = mainWindow->getUi();
  ui->textBrowserApproachInfo->setSearchPaths({QApplication::applicationDirPath()});

  QFont f = ui->textBrowserApproachInfo->font();
  float newSize = static_cast<float>(ui->textBrowserApproachInfo->font().pointSizeF()) *
                  OptionData::instance().getGuiInfoTextSize() / 100.f;
  if(newSize > 0.1f)
  {
    f.setPointSizeF(newSize);
    ui->textBrowserApproachInfo->setFont(f);
  }
}

ApproachTreeController::~ApproachTreeController()
{
  delete infoBuilder;
}

void ApproachTreeController::showApproaches(maptypes::MapAirport airport)
{
  Ui::MainWindow *ui = mainWindow->getUi();
  ui->dockWidgetRoute->show();
  ui->dockWidgetRoute->raise();
  ui->tabWidgetRoute->setCurrentIndex(1);

  if(currentAirport.id == airport.id && !approachViewMode)
    // Ignore if noting has changed - or jump out of the view mode
    return;

  emit approachLegSelected(maptypes::MapApproachRef());
  emit approachSelected(maptypes::MapApproachRef());

  if(approachViewMode)
  {
    currentAirport = airport;

    // Change mode
    disableViewMode();
  }
  else
  {
    // Put state on stack and update tree
    if(currentAirport.id != -1)
      recentTreeState.insert(currentAirport.id, saveTreeViewState());

    currentAirport = airport;

    fillApproachTreeWidget();

    restoreTreeViewState(recentTreeState.value(currentAirport.id));
  }
}

void ApproachTreeController::anchorClicked(const QUrl& url)
{
  if(url.scheme() == "lnm")
    emit showRect(currentAirport.bounding, false);
}

void ApproachTreeController::fillApproachInformation(const maptypes::MapAirport& airport,
                                                     const maptypes::MapApproachRef& ref)
{
  atools::util::HtmlBuilder html(true);
  infoBuilder->approachText(airport, html, QApplication::palette().color(QPalette::Active, QPalette::Base), ref);

  Ui::MainWindow *ui = mainWindow->getUi();
  ui->textBrowserApproachInfo->clear();
  ui->textBrowserApproachInfo->setText(html.getHtml());
}

void ApproachTreeController::fillApproachTreeWidget()
{
  treeWidget->clearSelection();
  treeWidget->clear();
  itemIndex.clear();
  itemLoadedIndex.clear();

  if(currentAirport.id != -1)
  {
    if(approachViewMode)
    {
      // Show information for the selected approach and/or transition
      fillApproachInformation(currentAirport, approachViewModeRef);

      // Change the tree widget to look more like a table view
      treeWidget->setStyleSheet(
        QString("QTreeView::item::!selected { border: 0.5px; border-style: solid; border-color: %1;}").
        arg(QApplication::palette().color(QPalette::Active, QPalette::Window).name()));
      treeWidget->setIndentation(0);

      // Add only legs in view mode
      QTreeWidgetItem *root = treeWidget->invisibleRootItem();
      if(approachViewModeRef.isApproachOnly())
      {
        const MapApproachLegs *legs = approachQuery->getApproachLegs(currentAirport, approachViewModeRef.approachId);
        addApproachLegs(legs, root);
      }
      else if(approachViewModeRef.isApproachAndTransition())
      {
        const MapApproachLegs *legs = approachQuery->getTransitionLegs(currentAirport, approachViewModeRef.transitionId);
        addTransitionLegs(legs, root);
        addApproachLegs(legs, root);
      }
    }
    else
    {
      // Show all approaches in the text information
      fillApproachInformation(currentAirport, maptypes::MapApproachRef());

      // Reset table like style back to plain tree
      treeWidget->setStyleSheet(QString());
      treeWidget->resetIndentation();

      // Add a tree of transitions and approaches
      const SqlRecordVector *recAppVector = infoQuery->getApproachInformation(currentAirport.id);
      if(recAppVector != nullptr)
      {
        QTreeWidgetItem *root = treeWidget->invisibleRootItem();

        for(const SqlRecord& recApp : *recAppVector)
        {
          int runwayEndId = recApp.valueInt("runway_end_id");
          int apprId = recApp.valueInt("approach_id");

          itemIndex.append(MapApproachRef(currentAirport.id, runwayEndId, apprId));

          QTreeWidgetItem *apprItem = buildApprItem(root, recApp);

          const SqlRecordVector *recTransVector = infoQuery->getTransitionInformation(recApp.valueInt("approach_id"));
          if(recTransVector != nullptr)
          {
            // Transitions for this approach
            for(const SqlRecord& recTrans : *recTransVector)
            {
              itemIndex.append(MapApproachRef(currentAirport.id, runwayEndId, apprId, recTrans.valueInt("transition_id")));
              buildTransItem(apprItem, recTrans);
            }
          }
        }
      }
      itemLoadedIndex.resize(itemIndex.size());
    }
  }

  if(itemIndex.isEmpty())
  {
    if(currentAirport.id == -1)
    {
      QTreeWidgetItem *item = new QTreeWidgetItem(treeWidget->invisibleRootItem(), {tr("No airport selected.")});
      item->setDisabled(true);
      item->setFirstColumnSpanned(true);
    }
    else
    {
      QTreeWidgetItem *item = new QTreeWidgetItem(treeWidget->invisibleRootItem(),
                                                  {tr("%1 has no approaches.").
                                                   arg(maptypes::airportText(currentAirport))}, -1);
      item->setDisabled(true);
      item->setFirstColumnSpanned(true);
    }
  }
}

void ApproachTreeController::saveState()
{
  Ui::MainWindow *ui = mainWindow->getUi();
  atools::gui::WidgetState(lnm::APPROACHTREE_WIDGET).save({ui->actionInfoApproachShowAppr,
                                                           ui->actionInfoApproachShowMissedAppr,
                                                           ui->actionInfoApproachShowTrans,
                                                           ui->splitterApproachInfo,
                                                           ui->tabWidgetRoute,
                                                           ui->treeWidgetApproachInfo});

  atools::settings::Settings& settings = atools::settings::Settings::instance();
  if(approachViewMode)
    // Use last saved state before entering view mode
    settings.setValueVar(lnm::APPROACHTREE_STATE, recentTreeState.value(currentAirport.id));
  else
  {
    // Use current state and update the map too
    QBitArray state = saveTreeViewState();
    recentTreeState.insert(currentAirport.id, state);
    settings.setValueVar(lnm::APPROACHTREE_STATE, state);
  }

  settings.setValueVar(lnm::APPROACHTREE_SELECTED_APPR + "_Mode", approachViewMode);
  settings.setValueVar(lnm::APPROACHTREE_SELECTED_APPR + "_AirportId", approachViewModeRef.airportId);
  settings.setValueVar(lnm::APPROACHTREE_SELECTED_APPR + "_RunwayEndId", approachViewModeRef.runwayEndId);
  settings.setValueVar(lnm::APPROACHTREE_SELECTED_APPR + "_ApproachId", approachViewModeRef.approachId);
  settings.setValueVar(lnm::APPROACHTREE_SELECTED_APPR + "_TransitionId", approachViewModeRef.transitionId);
  settings.setValueVar(lnm::APPROACHTREE_SELECTED_APPR + "_LegId", approachViewModeRef.legId);

  settings.setValue(lnm::APPROACHTREE_AIRPORT, currentAirport.id);
}

void ApproachTreeController::restoreState()
{
  Ui::MainWindow *ui = mainWindow->getUi();
  atools::gui::WidgetState(lnm::APPROACHTREE_WIDGET).restore({ui->actionInfoApproachShowAppr,
                                                              ui->actionInfoApproachShowMissedAppr,
                                                              ui->actionInfoApproachShowTrans,
                                                              ui->splitterApproachInfo,
                                                              ui->tabWidgetRoute,
                                                              ui->treeWidgetApproachInfo});

  atools::settings::Settings& settings = atools::settings::Settings::instance();

  mainWindow->getMapQuery()->getAirportById(currentAirport, settings.valueInt(lnm::APPROACHTREE_AIRPORT, -1));

  approachViewMode = settings.valueBool(lnm::APPROACHTREE_SELECTED_APPR + "_Mode", false);
  approachViewModeRef.airportId = settings.valueInt(lnm::APPROACHTREE_SELECTED_APPR + "_AirportId", -1);
  approachViewModeRef.runwayEndId = settings.valueInt(lnm::APPROACHTREE_SELECTED_APPR + "_RunwayEndId", -1);
  approachViewModeRef.approachId = settings.valueInt(lnm::APPROACHTREE_SELECTED_APPR + "_ApproachId", -1);
  approachViewModeRef.transitionId = settings.valueInt(lnm::APPROACHTREE_SELECTED_APPR + "_TransitionId", -1);
  approachViewModeRef.legId = settings.valueInt(lnm::APPROACHTREE_SELECTED_APPR + "_LegId", -1);

  fillApproachTreeWidget();

  QBitArray state = settings.valueVar(lnm::APPROACHTREE_STATE).toBitArray();
  recentTreeState.insert(currentAirport.id, state);
  if(approachViewMode)
    emit approachSelected(approachViewModeRef);
  else
    // Restoring state will emit above signal
    restoreTreeViewState(state);
}

void ApproachTreeController::itemSelectionChanged()
{
  QList<QTreeWidgetItem *> items = treeWidget->selectedItems();
  if(items.isEmpty())
  {
    emit approachSelected(maptypes::MapApproachRef());
    emit approachLegSelected(maptypes::MapApproachRef());
    fillApproachInformation(currentAirport, maptypes::MapApproachRef());
  }
  else
  {
    for(const QTreeWidgetItem *item : items)
    {
      const MapApproachRef& ref = itemIndex.at(item->type());

      qDebug() << Q_FUNC_INFO << ref.runwayEndId << ref.approachId << ref.transitionId << ref.legId;

      if(approachViewMode)
        emit approachLegSelected(ref);
      else
      {
        if(ref.isApproachOrTransition())
          emit approachSelected(ref);

        if(ref.isLeg())
          emit approachLegSelected(ref);
        else
          emit approachLegSelected(maptypes::MapApproachRef());

        if(ref.isApproachAndTransition())
        {
          QTreeWidgetItem *apprItem;
          if(ref.isLeg())
            apprItem = item->parent()->parent();
          else
            apprItem = item->parent();

          updateApproachItem(apprItem, ref.transitionId);
        }
      }
      // Show information for selected approach/transition in the text view
      fillApproachInformation(currentAirport, ref);
    }
  }
}

/* Update course and distance for the parent approach of this leg item */
void ApproachTreeController::updateApproachItem(QTreeWidgetItem *apprItem, int transitionId)
{
  if(apprItem != nullptr)
  {
    for(int i = 0; i < apprItem->childCount(); i++)
    {
      QTreeWidgetItem *child = apprItem->child(i);
      const MapApproachRef& childref = itemIndex.at(child->type());
      if(childref.isLeg())
      {
        const maptypes::MapApproachLegs *legs = approachQuery->getTransitionLegs(currentAirport, transitionId);
        if(legs != nullptr)
        {
          const maptypes::MapApproachLeg *aleg = legs->approachLegById(childref.legId);

          if(aleg != nullptr)
          {
            child->setText(COURSE, buildCourseStr(*aleg));
            child->setText(DISTANCE, buildDistanceStr(*aleg));
          }
          else
            qWarning() << "Approach legs not found" << childref.legId;
        }
        else
          qWarning() << "Transition not found" << transitionId;
      }
    }
  }
}

void ApproachTreeController::itemDoubleClicked(QTreeWidgetItem *item, int column)
{
  Q_UNUSED(column);
  showEntry(item, true);
}

void ApproachTreeController::itemExpanded(QTreeWidgetItem *item)
{
  if(approachViewMode)
    return;

  if(item != nullptr)
  {
    if(itemLoadedIndex.at(item->type()))
      return;

    // Get a copy since vector is rebuilt underneath
    const MapApproachRef ref = itemIndex.at(item->type());

    if(ref.legId == -1)
    {
      if(ref.approachId != -1 && ref.transitionId == -1)
      {
        const MapApproachLegs *legs = approachQuery->getApproachLegs(currentAirport, ref.approachId);
        addApproachLegs(legs, item);
        itemLoadedIndex.setBit(item->type());
      }
      else if(ref.approachId != -1 && ref.transitionId != -1)
      {
        const MapApproachLegs *legs = approachQuery->getTransitionLegs(currentAirport, ref.transitionId);
        addTransitionLegs(legs, item);
        itemLoadedIndex.setBit(item->type());
      }
      itemLoadedIndex.resize(itemIndex.size());
    }
  }
}

void ApproachTreeController::addApproachLegs(const MapApproachLegs *legs, QTreeWidgetItem *item)
{
  if(legs != nullptr)
  {
    for(const MapApproachLeg& leg : legs->approachLegs)
    {
      itemIndex.append(MapApproachRef(legs->ref.airportId, legs->ref.runwayEndId, legs->ref.approachId, -1, leg.legId));
      buildApprLegItem(item, leg);
    }
  }
}

void ApproachTreeController::addTransitionLegs(const MapApproachLegs *legs, QTreeWidgetItem *item)
{
  if(legs != nullptr)
  {
    for(const MapApproachLeg& leg : legs->transitionLegs)
    {
      itemIndex.append(MapApproachRef(legs->ref.airportId, legs->ref.runwayEndId, legs->ref.approachId,
                                      legs->ref.transitionId, leg.legId));
      buildTransLegItem(item, leg);
    }
  }
}

void ApproachTreeController::contextMenu(const QPoint& pos)
{
  qDebug() << Q_FUNC_INFO;

  QPoint menuPos = QCursor::pos();
  // Use widget center if position is not inside widget
  if(!treeWidget->rect().contains(treeWidget->mapFromGlobal(QCursor::pos())))
    menuPos = treeWidget->mapToGlobal(treeWidget->rect().center());

  // Save text which will be changed below
  Ui::MainWindow *ui = mainWindow->getUi();
  atools::gui::ActionTextSaver saver({ui->actionInfoApproachShow, ui->actionInfoApproachSelect});
  Q_UNUSED(saver);

  QTreeWidgetItem *item = treeWidget->itemAt(pos);

  ui->actionInfoApproachExpandAll->setDisabled(approachViewMode);
  ui->actionInfoApproachCollapseAll->setDisabled(approachViewMode);
  ui->actionInfoApproachClear->setDisabled(treeWidget->selectedItems().isEmpty());
  ui->actionInfoApproachUnselect->setDisabled(!approachViewMode);
  ui->actionInfoApproachSelect->setDisabled(item == nullptr || approachViewMode);
  ui->actionInfoApproachShow->setDisabled(item == nullptr);

  QMenu menu;
  menu.addAction(ui->actionInfoApproachExpandAll);
  menu.addAction(ui->actionInfoApproachCollapseAll);
  menu.addSeparator();
  menu.addAction(ui->actionInfoApproachClear);
  menu.addSeparator();
  if(approachViewMode)
    menu.addAction(ui->actionInfoApproachUnselect);
  else
    menu.addAction(ui->actionInfoApproachSelect);
  menu.addSeparator();
  menu.addAction(ui->actionInfoApproachShow);
  menu.addSeparator();
  menu.addAction(ui->actionInfoApproachShowAppr);
  menu.addAction(ui->actionInfoApproachShowMissedAppr);
  menu.addAction(ui->actionInfoApproachShowTrans);
  // menu.addAction(ui->actionInfoApproachAddToFlightPlan);

  QString text, showText;
  MapApproachRef ref;

  if(item != nullptr)
  {
    if(!approachViewMode)
    {
      ref = itemIndex.at(item->type());
      if(ref.isApproachAndTransition())
      {
        if(ref.isLeg())
          text = item->parent()->text(DESCRIPTION) + tr(" and ") + item->parent()->parent()->text(DESCRIPTION);
        else
          text = item->text(DESCRIPTION) + tr(" and ") + item->parent()->text(DESCRIPTION);
      }
      else if(ref.isApproachOnly())
      {
        if(ref.isLeg())
          text = item->parent()->text(DESCRIPTION);
        else
          text = item->text(DESCRIPTION);
      }
    }
    else
      text = item->text(DESCRIPTION);

    if(!text.isEmpty())
    {
      ui->actionInfoApproachShow->setEnabled(true);
      ui->actionInfoApproachSelect->setEnabled(true);
    }

    if(ref.isLeg())
      showText = item->text(IDENT).isEmpty() ? tr("Position") : item->text(IDENT);
    else
      showText = text;
  }

  ui->actionInfoApproachShow->setText(ui->actionInfoApproachShow->text().arg(showText));
  ui->actionInfoApproachSelect->setText(ui->actionInfoApproachSelect->text().arg(text));

  QAction *action = menu.exec(menuPos);
  if(action == ui->actionInfoApproachExpandAll)
  {
    // treeWidget->expandAll();
    const QTreeWidgetItem *root = treeWidget->invisibleRootItem();

    // First load child nodes to get the same tree
    for(int i = 0; i < root->childCount(); ++i)
      root->child(i)->setExpanded(true);
  }
  else if(action == ui->actionInfoApproachCollapseAll)
    treeWidget->collapseAll();
  else if(action == ui->actionInfoApproachClear)
  {
    treeWidget->clearSelection();
    emit approachLegSelected(maptypes::MapApproachRef());
    emit approachSelected(maptypes::MapApproachRef());
  }
  else if(action == ui->actionInfoApproachSelect)
    enableViewMode(ref);
  else if(action == ui->actionInfoApproachUnselect)
    disableViewMode();
  else if(action == ui->actionInfoApproachShow)
    showEntry(item, false);

  // Done by the actions themselves
  // else if(action == ui->actionInfoApproachShowAppr ||
  // action == ui->actionInfoApproachShowMissedAppr ||
  // action == ui->actionInfoApproachShowTrans)
}

void ApproachTreeController::showEntry(QTreeWidgetItem *item, bool doubleClick)
{
  if(item == nullptr)
    return;

  const MapApproachRef& ref = itemIndex.at(item->type());

  if(ref.legId != -1)
  {
    const maptypes::MapApproachLeg *leg = nullptr;

    if(ref.transitionId != -1)
      leg = approachQuery->getTransitionLeg(currentAirport, ref.legId);
    else if(ref.approachId != -1)
      leg = approachQuery->getApproachLeg(currentAirport, ref.legId);

    if(leg != nullptr)
      emit showPos(leg->line.getPos2(), 0.f, doubleClick);
  }
  else if(ref.transitionId != -1 && !doubleClick)
  {
    const maptypes::MapApproachLegs *legs = approachQuery->getTransitionLegs(currentAirport, ref.transitionId);
    if(legs != nullptr)
      emit showRect(legs->bounding, doubleClick);
  }
  else if(ref.approachId != -1 && !doubleClick)
  {
    const maptypes::MapApproachLegs *legs = approachQuery->getApproachLegs(currentAirport, ref.approachId);
    if(legs != nullptr)
      emit showRect(legs->bounding, doubleClick);
  }
}

QTreeWidgetItem *ApproachTreeController::buildApprItem(QTreeWidgetItem *runwayItem, const SqlRecord& recApp)
{
  QString suffix(recApp.valueStr("suffix"));
  QString type(recApp.valueStr("type"));
  int gpsOverlay = recApp.valueBool("has_gps_overlay");

  QString approachType;

  if(mainWindow->getCurrentSimulator() == atools::fs::FsPaths::P3D_V3 && type == "GPS" &&
     (suffix == "A" || suffix == "D") && gpsOverlay)
  {
    if(suffix == "A")
      approachType += tr("STAR");
    else if(suffix == "D")
      approachType += tr("SID");
  }
  else
  {
    approachType = tr("Approach ") + maptypes::approachType(type);

    if(!suffix.isEmpty())
      approachType += " " + suffix;

    if(gpsOverlay)
      approachType += tr(" (GPS Overlay)");
  }

  approachType += " " + recApp.valueStr("runway_name");

  QString altStr;
  if(recApp.valueFloat("altitude") > 0.f)
    altStr = Unit::altFeet(recApp.valueFloat("altitude"));

  QTreeWidgetItem *item = new QTreeWidgetItem({
                                                approachType,
                                                recApp.valueStr("fix_ident"),
                                                altStr
                                              }, itemIndex.size() - 1);
  item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);

  for(int i = 0; i < item->columnCount(); i++)
    item->setFont(i, approachFont);

  runwayItem->addChild(item);

  return item;
}

QTreeWidgetItem *ApproachTreeController::buildTransItem(QTreeWidgetItem *apprItem, const SqlRecord& recTrans)
{
  QString altStr;
  if(recTrans.valueFloat("altitude") > 0.f)
    altStr = Unit::altFeet(recTrans.valueFloat("altitude"));

  QString name(tr("Transition"));
  if(recTrans.valueStr("type") == "F")
    name.append(tr(" (Full)"));
  else if(recTrans.valueStr("type") == "D")
    name.append(tr(" (DME)"));

  QTreeWidgetItem *item = new QTreeWidgetItem({
                                                name,
                                                recTrans.valueStr("fix_ident"),
                                                altStr
                                              },
                                              itemIndex.size() - 1);
  item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);

  for(int i = 0; i < item->columnCount(); i++)
    item->setFont(i, transitionFont);

  apprItem->addChild(item);

  return item;
}

void ApproachTreeController::buildApprLegItem(QTreeWidgetItem *parentItem, const MapApproachLeg& leg)
{
  QString remarkStr = buildRemarkStr(leg);
  QTreeWidgetItem *item = new QTreeWidgetItem(
    {
      /*(leg.missed ? tr("Missed: ") : QString()) + */ maptypes::approachLegTypeStr(leg.type),
      leg.fixIdent,
      maptypes::altRestrictionText(leg.altRestriction),
      buildCourseStr(leg),
      buildDistanceStr(leg),
      remarkStr
    },
    itemIndex.size() - 1);

  item->setToolTip(REMARKS, remarkStr);

  setItemStyle(item, leg);

  parentItem->addChild(item);
}

void ApproachTreeController::buildTransLegItem(QTreeWidgetItem *parentItem, const MapApproachLeg& leg)
{
  QString remarkStr = buildRemarkStr(leg);
  QTreeWidgetItem *item = new QTreeWidgetItem(
    {
      maptypes::approachLegTypeStr(leg.type),
      leg.fixIdent,
      maptypes::altRestrictionText(leg.altRestriction),
      buildCourseStr(leg),
      buildDistanceStr(leg),
      remarkStr
    },
    itemIndex.size() - 1);

  item->setToolTip(REMARKS, remarkStr);

  setItemStyle(item, leg);

  parentItem->addChild(item);
}

void ApproachTreeController::setItemStyle(QTreeWidgetItem *item, const MapApproachLeg& leg)
{
  bool invalid = (!leg.fixIdent.isEmpty() && !leg.fixPos.isValid())
                 /* ||   (!leg.recFixIdent.isEmpty() && !leg.recFixPos.isValid())*/;

  for(int i = 0; i < item->columnCount(); i++)
  {
    if(!invalid)
    {
      item->setFont(i, leg.missed ? missedLegFont : legFont);
      if(leg.missed)
        item->setForeground(i, QColor(140, 140, 140));
    }
    else
    {
      item->setFont(i, invalidLegFont);
      item->setForeground(i, Qt::red);
    }
  }
}

QString ApproachTreeController::buildCourseStr(const MapApproachLeg& leg)
{
  QString courseStr;
  if(leg.course != 0.f && leg.type != maptypes::INITIAL_FIX && leg.type != maptypes::CONSTANT_RADIUS_ARC &&
     leg.type != maptypes::ARC_TO_FIX)
    courseStr = QLocale().toString(leg.course, 'f', 0) + (leg.trueCourse ? tr("°T") : tr("°M"));
  return courseStr;
}

QString ApproachTreeController::buildDistanceStr(const MapApproachLeg& leg)
{
  QString retval;

  if(leg.calculatedDistance > 0.f && leg.type != maptypes::INITIAL_FIX)
    retval += Unit::distNm(leg.calculatedDistance);
  else if(leg.distance > 0.f)
    retval += Unit::distNm(leg.distance);

  if(leg.time > 0.f)
  {
    if(!retval.isEmpty())
      retval += ", ";
    retval += QLocale().toString(leg.time, 'f', 0) + tr(" min");
  }

  return retval;
}

QString ApproachTreeController::buildRemarkStr(const MapApproachLeg& leg)
{
  QStringList remarks;
  if(leg.flyover)
    remarks.append(tr("Fly over"));

  if(leg.turnDirection == "R")
    remarks.append(tr("Turn right"));
  else if(leg.turnDirection == "L")
    remarks.append(tr("Turn left"));
  else if(leg.turnDirection == "B")
    remarks.append(tr("Turn left or right"));

  QString legremarks = maptypes::approachLegRemarks(leg.type);
  if(!legremarks.isEmpty())
    remarks.append(legremarks);

  if(!leg.recFixIdent.isEmpty())
  {
    if(leg.rho > 0.f)
      remarks.append(tr("%1 / %2 / %3").arg(leg.recFixIdent).
                     arg(Unit::distNm(leg.rho /*, true, 20, true*/)).
                     arg(QLocale().toString(leg.theta) + tr("°M")));
    else
      remarks.append(tr("%1").arg(leg.recFixIdent));
  }

  if(!leg.remarks.isEmpty())
    remarks.append(leg.remarks);

  if(!leg.fixIdent.isEmpty() && !leg.fixPos.isValid())
    remarks.append(tr("Data error: Fix %1/%2 not found").
                   arg(leg.fixIdent).arg(leg.fixRegion));
  if(!leg.recFixIdent.isEmpty() && !leg.recFixPos.isValid())
    remarks.append(tr("Data error: Recommended fix %1/%2 not found").
                   arg(leg.recFixIdent).arg(leg.recFixRegion));

  return remarks.join(", ");
}

QBitArray ApproachTreeController::saveTreeViewState()
{
  QList<const QTreeWidgetItem *> itemStack;
  const QTreeWidgetItem *root = treeWidget->invisibleRootItem();

  QBitArray state;

  if(!itemIndex.isEmpty() && !approachViewMode)
  {
    for(int i = 0; i < root->childCount(); ++i)
      itemStack.append(root->child(i));

    int itemIdx = 0;
    while(!itemStack.isEmpty())
    {
      const QTreeWidgetItem *item = itemStack.takeFirst();

      if(item->type() < itemIndex.size() && itemIndex.at(item->type()).legId != -1)
        // Do not save legs
        continue;

      bool selected = item->isSelected();

      // Check if a leg is selected and push selection status down to the approach or transition
      // This avoids the need of expanding during loading which messes up the order
      for(int i = 0; i < item->childCount(); i++)
      {
        if(itemIndex.at(item->child(i)->type()).legId != -1 && item->child(i)->isSelected())
        {
          selected = true;
          break;
        }
      }

      state.resize(itemIdx + 2);
      state.setBit(itemIdx, item->isExpanded()); // Fist bit in triple: expanded or not
      state.setBit(itemIdx + 1, selected); // Second bit: selection state

      qDebug() << item->text(DESCRIPTION) << item->text(IDENT) << "expanded" << item->isExpanded() << "selected" <<
      item->isSelected() << "child" << item->childCount();

      for(int i = 0; i < item->childCount(); ++i)
        itemStack.append(item->child(i));
      itemIdx += 2;
    }
  }
  return state;
}

void ApproachTreeController::restoreTreeViewState(const QBitArray& state)
{
  if(state.isEmpty())
    return;

  QList<QTreeWidgetItem *> itemStack;
  const QTreeWidgetItem *root = treeWidget->invisibleRootItem();

  // Find selected and expanded items first without tree modification to keep order
  for(int i = 0; i < root->childCount(); ++i)
    itemStack.append(root->child(i));
  int itemIdx = 0;
  QVector<QTreeWidgetItem *> itemsToExpand;
  QTreeWidgetItem *selectedItem = nullptr;
  while(!itemStack.isEmpty())
  {
    QTreeWidgetItem *item = itemStack.takeFirst();
    if(item != nullptr && itemIdx < state.size() - 1)
    {
      if(state.at(itemIdx))
        itemsToExpand.append(item);
      if(state.at(itemIdx + 1))
        selectedItem = item;

      for(int i = 0; i < item->childCount(); ++i)
        itemStack.append(item->child(i));
      itemIdx += 2;
    }
  }

  // Expand and possibly reload
  for(QTreeWidgetItem *item : itemsToExpand)
    item->setExpanded(true);

  // Center the selected item
  if(selectedItem != nullptr)
  {
    selectedItem->setSelected(true);
    treeWidget->scrollToItem(selectedItem, QAbstractItemView::PositionAtTop);
  }
}

void ApproachTreeController::enableViewMode(const maptypes::MapApproachRef& ref)
{
  // Save tree state
  recentTreeState.insert(currentAirport.id, saveTreeViewState());

  approachViewModeRef = ref;
  approachViewMode = true;

  fillApproachTreeWidget();
  emit approachSelected(approachViewModeRef);
}

void ApproachTreeController::disableViewMode()
{
  approachViewModeRef = MapApproachRef();
  approachViewMode = false;

  fillApproachTreeWidget();

  if(recentTreeState.contains(currentAirport.id))
    restoreTreeViewState(recentTreeState.value(currentAirport.id));

  // Selected signal will be emitted when selecting tree item
}
