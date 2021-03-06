/*****************************************************************************
* Copyright 2015-2019 Alexander Barthel alex@littlenavmap.org
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

#include "routecontroller.h"
#include "route/routestring.h"
#include "route/routestringdialog.h"

#include "navapp.h"
#include "options/optiondata.h"
#include "gui/helphandler.h"
#include "query/procedurequery.h"
#include "common/constants.h"
#include "common/tabindexes.h"
#include "fs/db/databasemeta.h"
#include "common/formatter.h"
#include "fs/perf/aircraftperf.h"
#include "search/proceduresearch.h"
#include "common/unit.h"
#include "exception.h"
#include "export/csvexporter.h"
#include "gui/actiontextsaver.h"
#include "gui/actionstatesaver.h"
#include "gui/errorhandler.h"
#include "gui/itemviewzoomhandler.h"
#include "gui/widgetstate.h"
#include "query/mapquery.h"
#include "query/airportquery.h"
#include "mapgui/mapwidget.h"
#include "parkingdialog.h"
#include "route/routefinder.h"
#include "route/routenetworkairway.h"
#include "route/routenetworkradio.h"
#include "settings/settings.h"
#include "ui_mainwindow.h"
#include "gui/dialog.h"
#include "route/routealtitude.h"
#include "route/routeexport.h"
#include "route/userwaypointdialog.h"
#include "route/flightplanentrybuilder.h"
#include "fs/pln/flightplanio.h"
#include "route/routestringdialog.h"
#include "util/htmlbuilder.h"
#include "common/symbolpainter.h"
#include "common/mapcolors.h"
#include "common/unit.h"
#include "common/unitstringtool.h"
#include "perf/aircraftperfcontroller.h"
#include "fs/sc/simconnectdata.h"

#include <QClipboard>
#include <QFile>
#include <QStandardItemModel>
#include <QInputDialog>
#include <QFileInfo>
#include <QTextTable>

namespace rc {
// Route table column indexes
enum RouteColumns
{
  FIRST_COLUMN,
  IDENT = FIRST_COLUMN,
  REGION,
  NAME,
  PROCEDURE,
  AIRWAY_OR_LEGTYPE,
  RESTRICTION,
  TYPE,
  FREQ,
  RANGE,
  COURSE,
  DIRECT,
  DIST,
  REMAINING_DISTANCE,
  LEG_TIME,
  ETA,
  FUEL_WEIGHT,
  FUEL_VOLUME,
  REMARKS,
  LAST_COLUMN = REMARKS
};

}

using atools::fs::pln::Flightplan;
using atools::fs::pln::FlightplanEntry;
using namespace atools::geo;

namespace pln = atools::fs::pln;

RouteController::RouteController(QMainWindow *parentWindow, QTableView *tableView)
  : QObject(parentWindow), mainWindow(parentWindow), view(tableView)
{
  mapQuery = NavApp::getMapQuery();
  airportQuery = NavApp::getAirportQuerySim();

  routeColumns = QList<QString>({QObject::tr("Ident"),
                                 QObject::tr("Region"),
                                 QObject::tr("Name"),
                                 QObject::tr("Procedure"),
                                 QObject::tr("Airway or\nProcedure"),
                                 QObject::tr("Restriction\n%alt%/%speed%"),
                                 QObject::tr("Type"),
                                 QObject::tr("Freq.\nMHz/kHz/Cha."),
                                 QObject::tr("Range\n%dist%"),
                                 QObject::tr("Course\n°M"),
                                 QObject::tr("Direct\n°M"),
                                 QObject::tr("Distance\n%dist%"),
                                 QObject::tr("Remaining\n%dist%"),
                                 QObject::tr("Leg Time\nhh:mm"),
                                 QObject::tr("ETA\nhh:mm"),
                                 QObject::tr("Fuel Rem.\n%weight%"),
                                 QObject::tr("Fuel Rem.\n%volume%"),
                                 QObject::tr("Remarks")});

  flightplanIO = new atools::fs::pln::FlightplanIO();

  Ui::MainWindow *ui = NavApp::getMainUi();
  // Update units
  units = new UnitStringTool();
  units->init({
    ui->spinBoxRouteAlt,
    ui->spinBoxAircraftPerformanceWindSpeed
  });

  ui->labelRouteError->setVisible(false);

  // Set default table cell and font size to avoid Qt overly large cell sizes
  zoomHandler = new atools::gui::ItemViewZoomHandler(view);
  connect(NavApp::navAppInstance(), &atools::gui::Application::fontChanged, this, &RouteController::fontChanged);

  entryBuilder = new FlightplanEntryBuilder();

  symbolPainter = new SymbolPainter();

  // Use saved font size for table view
  zoomHandler->zoomPercent(OptionData::instance().getGuiRouteTableTextSize());
  updateIcons();

  view->setContextMenuPolicy(Qt::CustomContextMenu);

  // Create flight plan calculation caches
  routeNetworkRadio = new RouteNetworkRadio(NavApp::getDatabaseNav());
  routeNetworkAirway = new RouteNetworkAirway(NavApp::getDatabaseNav());

  // Set up undo/redo framework
  undoStack = new QUndoStack(mainWindow);
  undoStack->setUndoLimit(ROUTE_UNDO_LIMIT);

  QAction *undoAction = undoStack->createUndoAction(mainWindow, tr("&Undo Flight Plan"));
  undoAction->setIcon(QIcon(":/littlenavmap/resources/icons/undo.svg"));
  undoAction->setShortcut(QKeySequence("Ctrl+Z"));

  QAction *redoAction = undoStack->createRedoAction(mainWindow, tr("&Redo Flight Plan"));
  redoAction->setIcon(QIcon(":/littlenavmap/resources/icons/redo.svg"));
  redoAction->setShortcut(QKeySequence("Ctrl+Y"));

  connect(redoAction, &QAction::triggered, this, &RouteController::redoTriggered);
  connect(undoAction, &QAction::triggered, this, &RouteController::undoTriggered);

  ui->toolBarRoute->insertAction(ui->actionRouteSelectParking, undoAction);
  ui->toolBarRoute->insertAction(ui->actionRouteSelectParking, redoAction);

  ui->menuRoute->insertActions(ui->actionRouteSelectParking, {undoAction, redoAction});
  ui->menuRoute->insertSeparator(ui->actionRouteSelectParking);

  connect(ui->spinBoxRouteAlt, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this, &RouteController::routeAltChanged);
  connect(ui->comboBoxRouteType, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
          this, &RouteController::routeTypeChanged);

  connect(view, &QTableView::doubleClicked, this, &RouteController::doubleClick);
  connect(view, &QTableView::customContextMenuRequested, this, &RouteController::tableContextMenu);

  connect(&routeAltDelayTimer, &QTimer::timeout, this, &RouteController::routeAltChangedDelayed);
  routeAltDelayTimer.setSingleShot(true);

  // set up table view
  view->horizontalHeader()->setSectionsMovable(true);
  view->verticalHeader()->setSectionsMovable(false);
  view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

  model = new QStandardItemModel();
  QItemSelectionModel *m = view->selectionModel();
  view->setModel(model);
  delete m;

  // Avoid stealing of keys from other default menus
  ui->actionRouteLegDown->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteLegUp->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteDeleteLeg->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteShowInformation->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteShowApproaches->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteShowOnMap->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteTableSelectNothing->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteTableSelectAll->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteActivateLeg->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteSetMark->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteResetView->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteTableCopy->setShortcutContext(Qt::WidgetWithChildrenShortcut);

  // Add action/shortcuts to table view
  view->addActions({ui->actionRouteLegDown, ui->actionRouteLegUp, ui->actionRouteDeleteLeg,
                    ui->actionRouteTableCopy, ui->actionRouteShowInformation, ui->actionRouteShowApproaches,
                    ui->actionRouteShowOnMap, ui->actionRouteTableSelectNothing, ui->actionRouteTableSelectAll,
                    ui->actionRouteActivateLeg, ui->actionRouteResetView, ui->actionRouteSetMark});

  void (RouteController::*selChangedPtr)(const QItemSelection &selected, const QItemSelection &deselected) =
    &RouteController::tableSelectionChanged;

  if(view->selectionModel() != nullptr)
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, selChangedPtr);

  // Connect actions - actions without shortcut key are used in the context menu method directly
  connect(ui->actionRouteTableCopy, &QAction::triggered, this, &RouteController::tableCopyClipboard);
  connect(ui->actionRouteLegDown, &QAction::triggered, this, &RouteController::moveSelectedLegsDown);
  connect(ui->actionRouteLegUp, &QAction::triggered, this, &RouteController::moveSelectedLegsUp);
  connect(ui->actionRouteDeleteLeg, &QAction::triggered, this, &RouteController::deleteSelectedLegs);

  connect(ui->actionRouteShowInformation, &QAction::triggered, this, &RouteController::showInformationMenu);
  connect(ui->actionRouteShowApproaches, &QAction::triggered, this, &RouteController::showProceduresMenu);
  connect(ui->actionRouteShowOnMap, &QAction::triggered, this, &RouteController::showOnMapMenu);

  connect(ui->dockWidgetRoute, &QDockWidget::visibilityChanged, this, &RouteController::dockVisibilityChanged);
  connect(ui->actionRouteTableSelectNothing, &QAction::triggered, this, &RouteController::clearSelection);
  connect(ui->actionRouteTableSelectAll, &QAction::triggered, this, &RouteController::selectAllTriggered);
  connect(ui->pushButtonRouteClearSelection, &QPushButton::clicked, this, &RouteController::clearSelection);
  connect(ui->pushButtonRouteHelp, &QPushButton::clicked, this, &RouteController::helpClicked);
  connect(ui->actionRouteActivateLeg, &QAction::triggered, this, &RouteController::activateLegTriggered);
}

RouteController::~RouteController()
{
  routeAltDelayTimer.stop();
  delete units;
  delete entryBuilder;
  delete model;
  delete undoStack;
  delete routeNetworkRadio;
  delete routeNetworkAirway;
  delete zoomHandler;
  delete symbolPainter;
  delete flightplanIO;
}

void RouteController::fontChanged()
{
  qDebug() << Q_FUNC_INFO;

  zoomHandler->fontChanged();
  optionsChanged();
}

void RouteController::undoTriggered()
{
  NavApp::setStatusMessage(QString(tr("Undo flight plan change.")));
}

void RouteController::redoTriggered()
{
  NavApp::setStatusMessage(QString(tr("Redo flight plan change.")));
}

/* Ctrl-C - copy selected table contents in CSV format to clipboard */
void RouteController::tableCopyClipboard()
{
  qDebug() << "RouteController::tableCopyClipboard";

  const Route& rt = route;
  QString csv;
  int exported = CsvExporter::selectionAsCsv(view, true /* rows */, true /* header */, csv, {"longitude", "latitude"},
                                             [&rt](int index) -> QStringList
  {
    return {QLocale().toString(rt.at(index).getPosition().getLonX(), 'f', 8),
            QLocale().toString(rt.at(index).getPosition().getLatY(), 'f', 8)};
  });

  if(!csv.isEmpty())
    QApplication::clipboard()->setText(csv);

  NavApp::setStatusMessage(QString(tr("Copied %1 entries to clipboard.")).arg(exported));
}

void RouteController::flightplanTableAsTextTable(QTextCursor& cursor, const QBitArray& selectedCols,
                                                 float fontPointSize) const
{
  int numCols = selectedCols.count(true);

  // Prepare table format ===================================
  QTextTableFormat fmt;
  fmt.setHeaderRowCount(1);
  fmt.setCellPadding(1);
  fmt.setCellSpacing(0);
  fmt.setBorder(2);
  fmt.setBorderBrush(Qt::lightGray);
  fmt.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
  QTextTable *table = cursor.insertTable(model->rowCount() + 1, numCols, fmt);

  // Cell alignment formats ===================================
  QTextBlockFormat alignRight;
  alignRight.setAlignment(Qt::AlignRight);
  QTextBlockFormat alignLeft;
  alignLeft.setAlignment(Qt::AlignLeft);

  // Text size and alternating background formats ===================================
  QTextCharFormat altFormat1 = table->cellAt(0, 0).format();
  altFormat1.setFontPointSize(fontPointSize);
  altFormat1.setBackground(mapcolors::mapPrintRowColor);

  QTextCharFormat altFormat2 = altFormat1;
  altFormat2.setBackground(mapcolors::mapPrintRowColorAlt);

  // Header font and background ================
  QTextCharFormat headerFormat = altFormat1;
  headerFormat.setFontWeight(QFont::Bold);
  headerFormat.setBackground(mapcolors::mapPrintHeaderColor);

  // Fill header =====================================================================
  // Table header from GUI widget
  QHeaderView *header = view->horizontalHeader();

  int cellIdx = 0;
  for(int col = 0; col < model->columnCount(); col++)
  {
    if(!selectedCols.at(col))
      // Ignore if not selected in the print dialog
      continue;

    table->cellAt(0, cellIdx).setFormat(headerFormat);
    cursor.setPosition(table->cellAt(0, cellIdx).firstPosition());
    QString txt = model->headerData(header->logicalIndex(col), Qt::Horizontal).toString().
                  replace("-\n", "-").replace("\n", " ");
    cursor.insertText(txt);
    cellIdx++;
  }

  // Fill table =====================================================================
  for(int row = 0; row < model->rowCount(); row++)
  {
    cellIdx = 0;
    for(int col = 0; col < model->columnCount(); col++)
    {
      if(!selectedCols.at(col))
        // Ignore if not selected in the print dialog
        continue;

      const QStandardItem *item = model->item(row, header->logicalIndex(col));

      if(item != nullptr)
      {
        // Alternating background =============================
        QTextCharFormat textFormat = (row % 2) == 0 ? altFormat1 : altFormat2;

        // Determine font color base on leg =============
        const RouteLeg& leg = route.at(row);
        if(leg.isAnyProcedure())
          textFormat.setForeground(leg.getProcedureLeg().isMissed() ?
                                   mapcolors::routeProcedureMissedTableColor :
                                   mapcolors::routeProcedureTableColor);
        else if((col == rc::IDENT && leg.getMapObjectType() == map::INVALID) ||
                (col == rc::AIRWAY_OR_LEGTYPE && leg.isRoute() && leg.isAirwaySetAndInvalid()))
          textFormat.setForeground(Qt::red);
        else
          textFormat.setForeground(Qt::black);

        if(col == 0)
          // Make ident bold
          textFormat.setFontWeight(QFont::Bold);

        table->cellAt(row + 1, cellIdx).setFormat(textFormat);
        cursor.setPosition(table->cellAt(row + 1, cellIdx).firstPosition());

        // Assign alignment to cell
        if(item->textAlignment() == Qt::AlignRight)
          cursor.setBlockFormat(alignRight);
        else
          cursor.setBlockFormat(alignLeft);

        cursor.insertText(item->text());
      }
      cellIdx++;
    }
  }

  // Move cursor after table
  cursor.setPosition(table->lastPosition() + 1);
}

void RouteController::flightplanHeader(atools::util::HtmlBuilder& html, bool titleOnly) const
{
  html.text(buildFlightplanLabel(true /* print */, titleOnly), atools::util::html::NO_ENTITIES);

  if(!titleOnly)
    html.p(buildFlightplanLabel2(), atools::util::html::NO_ENTITIES | atools::util::html::BIG);
}

QString RouteController::getFlightplanTableAsHtml(float iconSizePixel) const
{
  qDebug() << Q_FUNC_INFO;
  using atools::util::HtmlBuilder;

  atools::util::HtmlBuilder html(mapcolors::webTableBackgroundColor, mapcolors::webTableAltBackgroundColor);
  int minColWidth = view->horizontalHeader()->minimumSectionSize() + 1;

  // Header lines
  html.p(buildFlightplanLabel(true /* print */), atools::util::html::NO_ENTITIES | atools::util::html::BIG);
  html.p(buildFlightplanLabel2(), atools::util::html::NO_ENTITIES | atools::util::html::BIG);
  html.table();

  // Table header
  QHeaderView *header = view->horizontalHeader();
  html.tr(Qt::lightGray);
  html.th(QString()); // Icon
  for(int col = 0; col < model->columnCount(); col++)
  {
    if(view->columnWidth(header->logicalIndex(col)) > minColWidth)
      html.th(model->headerData(header->logicalIndex(col), Qt::Horizontal).
              toString().replace("-\n", "-<br/>").replace("\n", "<br/>"), atools::util::html::NO_ENTITIES);
  }
  html.trEnd();

  int nearestLegIndex = route.getActiveLegIndexCorrected();

  // Table content
  for(int row = 0; row < model->rowCount(); row++)
  {
    // First column is icon
    html.tr(nearestLegIndex != row ? QColor() : mapcolors::nextWaypointColor);
    const RouteLeg& routeLeg = route.at(row);

    if(iconSizePixel > 0.f)
    {
      int sizeInt = atools::roundToInt(iconSizePixel);

      html.td();
      html.img(iconForLeg(routeLeg, iconSizePixel), QString(), QString(), QSize(sizeInt, sizeInt));
      html.tdEnd();
    }

    // Rest of columns
    for(int col = 0; col < model->columnCount(); col++)
    {
      if(view->columnWidth(header->logicalIndex(col)) > minColWidth)
      {
        QStandardItem *item = model->item(row, header->logicalIndex(col));

        if(item != nullptr)
        {
          if(item->textAlignment().testFlag(Qt::AlignRight))
            html.td(item->text().toHtmlEscaped(), atools::util::html::ALIGN_RIGHT);
          else
            html.td(item->text().toHtmlEscaped());
        }
        else
          html.td(QString());
      }
    }
    html.trEnd();
  }
  html.tableEnd();
  return html.getHtml();
}

void RouteController::routeStringToClipboard() const
{
  qDebug() << Q_FUNC_INFO;

  QString str = RouteString::createStringForRoute(route,
                                                  NavApp::getRouteCruiseSpeedKts(),
                                                  RouteStringDialog::getOptionsFromSettings());

  qDebug() << "route string" << str;
  if(!str.isEmpty())
    QApplication::clipboard()->setText(str);

  NavApp::setStatusMessage(QString(tr("Flight plan string to clipboard.")));
}

void RouteController::aircraftPerformanceChanged()
{
  qDebug() << Q_FUNC_INFO;
  if(!route.isEmpty())
  {
    // Get type, speed and cruise altitude from widgets
    updateTableHeaders(); // Update lbs/gal for fuel
    updateFlightplanFromWidgets();
    route.updateLegAltitudes();

    updateModelRouteTimeFuel();

    highlightProcedureItems();
    highlightNextWaypoint(route.getActiveLegIndexCorrected());
    updateErrorLabel();
  }
  updateWindowLabel();
  NavApp::updateWindowTitle();

  // Emit also for empty route to catch performance changes
  emit routeChanged(true);
}

/* Spin box altitude has changed value */
void RouteController::routeAltChanged()
{
  RouteCommand *undoCommand = nullptr;

  if(!route.isEmpty() /*&& route.getFlightplan().canSaveAltitude()*/)
    undoCommand = preChange(tr("Change Altitude"), rctype::ALTITUDE);

  // Get type, speed and cruise altitude from widgets
  updateFlightplanFromWidgets();

  postChange(undoCommand);

  updateWindowLabel();
  NavApp::updateWindowTitle();

  // Calls RouteController::routeAltChangedDelayed
  routeAltDelayTimer.start(ROUTE_ALT_CHANGE_DELAY_MS);
}

void RouteController::routeAltChangedDelayed()
{
  route.updateLegAltitudes();

  // Update performance
  updateModelRouteTimeFuel();
  updateErrorLabel();
  updateWindowLabel();

  // Delay change to avoid hanging spin box when profile updates
  emit routeAltitudeChanged(route.getCruisingAltitudeFeet());
}

/* Combo box route type has value changed */
void RouteController::routeTypeChanged()
{
  RouteCommand *undoCommand = nullptr;

  if(!route.isEmpty() /*&& route.getFlightplan().canSaveFlightplanType()*/)
    undoCommand = preChange(tr("Change Type"));

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  postChange(undoCommand);

  NavApp::updateWindowTitle();

  if(!route.isEmpty())
  {
    emit routeChanged(false);
    Ui::MainWindow *ui = NavApp::getMainUi();
    NavApp::setStatusMessage(tr("Flight plan type changed to %1.").arg(ui->comboBoxRouteType->currentText()));
  }
}

bool RouteController::selectDepartureParking()
{
  qDebug() << Q_FUNC_INFO;

  const map::MapAirport& airport = route.first().getAirport();
  ParkingDialog dialog(mainWindow, airport);

  int result = dialog.exec();
  dialog.hide();

  if(result == QDialog::Accepted)
  {
    // Set either start of parking
    map::MapParking parking;
    map::MapStart start;
    if(dialog.getSelectedParking(parking))
    {
      routeSetParking(parking);
      return true;
    }
    else if(dialog.getSelectedStartPosition(start))
    {
      routeSetStartPosition(start);
      return true;
    }
  }
  return false;
}

void RouteController::saveState()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  atools::gui::WidgetState(lnm::ROUTE_VIEW).save({view, ui->comboBoxRouteType,
                                                  ui->spinBoxRouteAlt,
                                                  ui->actionRouteFollowSelection,
                                                  ui->tabWidgetRoute});

  atools::settings::Settings::instance().setValue(lnm::ROUTE_FILENAME, routeFilename);
}

void RouteController::updateTableHeaders()
{
  using atools::fs::perf::AircraftPerf;

  QList<QString> routeHeaders(routeColumns);

  for(QString& str : routeHeaders)
    str = Unit::replacePlaceholders(str);

  model->setHorizontalHeaderLabels(routeHeaders);
}

void RouteController::restoreState()
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  updateTableHeaders();

  atools::gui::WidgetState state(lnm::ROUTE_VIEW, true, true);
  state.restore({view, ui->comboBoxRouteType, ui->spinBoxRouteAlt, ui->actionRouteFollowSelection,
                 ui->tabWidgetRoute});

  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_ROUTE)
  {
    QString newRouteFilename = atools::settings::Settings::instance().valueStr(lnm::ROUTE_FILENAME);

    if(!newRouteFilename.isEmpty())
    {
      if(QFile::exists(newRouteFilename))
      {
        if(!loadFlightplan(newRouteFilename))
        {
          // Cannot be loaded - clear current filename
          routeFilename.clear();
          fileDeparture.clear();
          fileDestination.clear();
          fileIfrVfr = pln::VFR;
          route.clear();
          routeFileFormat = atools::fs::pln::PLN_FSX;
        }
      }
      else
      {
        routeFilename.clear();
        fileDeparture.clear();
        fileDestination.clear();
        fileIfrVfr = pln::VFR;
        route.clear();
        routeFileFormat = atools::fs::pln::PLN_FSX;
      }
    }
  }

  if(route.isEmpty())
    updateFlightplanFromWidgets();

  units->update();
}

void RouteController::getSelectedRouteLegs(QList<int>& selLegIndexes) const
{
  if(NavApp::getMainUi()->dockWidgetRoute->isVisible())
  {
    if(view->selectionModel() != nullptr)
    {
      QItemSelection sm = view->selectionModel()->selection();
      for(const QItemSelectionRange& rng : sm)
      {
        for(int row = rng.top(); row <= rng.bottom(); ++row)
          selLegIndexes.append(row);
      }
    }
  }
}

void RouteController::newFlightplan()
{
  qDebug() << "newFlightplan";
  clearRoute();

  // Copy current alt and type from widgets to flightplan
  updateFlightplanFromWidgets();

  route.createRouteLegsFromFlightplan();
  route.updateAll();

  updateTableModel();
  NavApp::updateWindowTitle();
  updateErrorLabel();
  emit routeChanged(true /* geometry changed */, true /* new flight plan */);
}

void RouteController::loadFlightplan(atools::fs::pln::Flightplan flightplan, const QString& filename,
                                     bool quiet, bool changed, bool adjustAltitude)
{
  qDebug() << Q_FUNC_INFO << filename;

  bool adjustRouteType = false;
#ifdef DEBUG_INFORMATION
  qDebug() << flightplan;
#endif

  if(flightplan.getFileFormat() == atools::fs::pln::FLP)
  {
    // FLP is nothing more than a sort of route string
    // New waypoints along airways have to be inserted and waypoints have to be resolved without coordinate backup

    // Create a route string
    QStringList routeString;
    for(int i = 0; i < flightplan.getEntries().size(); i++)
    {
      const FlightplanEntry& entry = flightplan.at(i);
      if(!entry.getAirway().isEmpty())
        routeString.append(entry.getAirway());
      routeString.append(entry.getIcaoIdent());
    }
    qInfo() << "FLP generated route string" << routeString;

    // All is valid except the waypoint entries
    flightplan.getEntries().clear();

    // Use route string to overwrite the current incomplete flight plan object
    RouteString rs(entryBuilder);
    rs.setPlaintextMessages(true);
    bool ok = rs.createRouteFromString(routeString.join(" "), flightplan);
    qInfo() << "createRouteFromString messages" << rs.getMessages();

    if(!ok)
    {
      atools::gui::Dialog::warning(mainWindow,
                                   tr("Loading of FLP flight plan failed:<br/><br/>") + rs.getMessages().join("<br/>"));
      return;

    }
    else if(!rs.getMessages().isEmpty())
      atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOW_LOAD_FLP_WARN,
                                                     tr("Warnings while loading FLP flight plan file:<br/><br/>") +
                                                     rs.getMessages().join("<br/>"),
                                                     tr("Do not &show this dialog again."));

    // Copy speed, type and altitude from GUI
    updateFlightplanFromWidgets(flightplan);
    adjustAltitude = true; // Change altitude based on airways later
    adjustRouteType = true;
  }
  else if(flightplan.getFileFormat() == atools::fs::pln::FMS11 ||
          flightplan.getFileFormat() == atools::fs::pln::FMS3 ||
          flightplan.getFileFormat() == atools::fs::pln::PLN_FSC ||
          flightplan.getFileFormat() == atools::fs::pln::FLIGHTGEAR)
  {
    // Save altitude
    int cruiseAlt = flightplan.getCruisingAltitude();

    // Set type cruise altitude and speed since FMS and FSC do not support this
    updateFlightplanFromWidgets(flightplan);

    // Reset altitude after update from widgets
    if(cruiseAlt > 0)
      flightplan.setCruisingAltitude(cruiseAlt);
    else
      adjustAltitude = true; // Change altitude based on airways later

    adjustRouteType = true;
  }

  clearRoute();

  if(changed)
    undoIndexClean = -1;

  route.setFlightplan(flightplan);

  routeFilename = filename;
  routeFileFormat = flightplan.getFileFormat();
  fileDeparture = flightplan.getDepartureIdent();
  fileDestination = flightplan.getDestinationIdent();
  fileIfrVfr = flightplan.getFlightplanType();

  assignAircraftPerformance(flightplan);

  route.createRouteLegsFromFlightplan();

  loadProceduresFromFlightplan(true /* clear old procedure properties */, false /* quiet */);
  route.updateAll();
  route.updateAirwaysAndAltitude(adjustAltitude, adjustRouteType);

  // Need to update again after updateAll and altitude change
  route.updateLegAltitudes();

  // Get number from user waypoint from user defined waypoint in fs flight plan
  entryBuilder->setCurUserpointNumber(route.getNextUserWaypointNumber());

  // Update start position for other formats than FSX/P3D
  bool forceUpdate = flightplan.getFileFormat() != atools::fs::pln::PLN_FSX;

  // Do not create an entry on the undo stack since this plan file type does not support it
  if(updateStartPositionBestRunway(forceUpdate /* force */, false /* undo */))
  {
    if(flightplan.getFileFormat() != atools::fs::pln::PLN_FSX ? false : !quiet)
    {
      NavApp::deleteSplashScreen();

      atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOWROUTE_START_CHANGED,
                                                     tr("The flight plan had no valid start position.\n"
                                                        "The start position is now set to the longest "
                                                        "primary runway of the departure airport."),
                                                     tr("Do not &show this dialog again."));
    }
  }

  updateTableModel();
  updateErrorLabel();
  NavApp::updateWindowTitle();

#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << route;
#endif

  emit routeChanged(true /* geometry changed */, true /* new flight plan */);
}

/* Fill the route procedure legs structures with data based on the procedure properties in the flight plan */
void RouteController::loadProceduresFromFlightplan(bool clearOldProcedureProperties, bool quiet)
{
  if(route.isEmpty())
    return;

  QStringList procedureLoadingErrors;
  proc::MapProcedureLegs arrival, departure, star;
  NavApp::getProcedureQuery()->getLegsForFlightplanProperties(route.getFlightplan().getProperties(),
                                                              route.first().getAirport(),
                                                              route.last().getAirport(),
                                                              arrival, star, departure, procedureLoadingErrors);
  // SID/STAR with multiple runways are already assigned
  route.setDepartureProcedureLegs(departure);
  route.setStarProcedureLegs(star);
  route.setArrivalProcedureLegs(arrival);
  route.updateProcedureLegs(entryBuilder, clearOldProcedureProperties);

  if(!quiet && !procedureLoadingErrors.isEmpty())
  {
    NavApp::deleteSplashScreen();
    atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOWROUTE_PROC_ERROR,
                                                   tr("<p>Cannot load procedures into flight plan:</p>"
                                                        "<ul><li>%1</li></ul>").
                                                   arg(procedureLoadingErrors.join("</li><li>")),
                                                   tr("Do not &show this dialog again."));
  }
}

bool RouteController::loadFlightplan(const QString& filename)
{
  Flightplan newFlightplan;
  try
  {
    qDebug() << Q_FUNC_INFO << "loadFlightplan" << filename;
    // Will throw an exception if something goes wrong
    flightplanIO->load(newFlightplan, filename);
    // qDebug() << "Flight plan custom data" << newFlightplan.getProperties();

    // Convert altitude to local unit
    newFlightplan.setCruisingAltitude(atools::roundToInt(Unit::altFeetF(newFlightplan.getCruisingAltitude())));

    loadFlightplan(newFlightplan, filename, false /*quiet*/, false /*changed*/, false /*adjust alt*/);
  }
  catch(atools::Exception& e)
  {
    NavApp::deleteSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    NavApp::deleteSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteController::insertFlightplan(const QString& filename, int insertBefore)
{
  qDebug() << Q_FUNC_INFO << filename << insertBefore;

  Flightplan flightplan;
  try
  {
    // Will throw an exception if something goes wrong
    flightplanIO->load(flightplan, filename);

    // Convert altitude to local unit
    flightplan.setCruisingAltitude(atools::roundToInt(Unit::altFeetF(flightplan.getCruisingAltitude())));

    RouteCommand *undoCommand = preChange(insertBefore >= route.size() ? tr("Insert") : tr("Append"));

    bool beforeDestInsert = false, beforeDepartPrepend = false, afterDestAppend = false, middleInsert = false;
    int insertPosSelection = insertBefore;

    if(insertBefore >= route.size())
    {
      // Append ================================================================
      afterDestAppend = true;

      // Remove arrival legs and properties
      route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

      // Start of selection without arrival procedures
      insertPosSelection = route.size();

      // Append flight plan to current flightplan object - route is updated later
      for(const FlightplanEntry& entry : flightplan.getEntries())
        route.getFlightplan().getEntries().append(entry);

      // Appended after destination airport
      route.getFlightplan().setDestinationAiportName(flightplan.getDestinationAiportName());
      route.getFlightplan().setDestinationIdent(flightplan.getDestinationIdent());
      route.getFlightplan().setDestinationPosition(flightplan.getDestinationPosition());

      // Copy any STAR and arrival procedure properties from the loaded plan
      atools::fs::pln::copyArrivalProcedureProperties(route.getFlightplan().getProperties(),
                                                      flightplan.getProperties());
      atools::fs::pln::copyStarProcedureProperties(route.getFlightplan().getProperties(),
                                                   flightplan.getProperties());
    }
    else
    {
      // Insert ================================================================
      if(insertBefore == 0)
      {
        // Insert before departure ==============
        beforeDepartPrepend = true;

        // Remove departure legs and properties
        route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);

        // Added before departure airport
        route.getFlightplan().setDepartureAiportName(flightplan.getDepartureAiportName());
        route.getFlightplan().setDepartureIdent(flightplan.getDepartureIdent());
        route.getFlightplan().setDeparturePosition(flightplan.getDeparturePosition(),
                                                   flightplan.getEntries().first().getPosition().getAltitude());

        // Copy SID properties from source
        atools::fs::pln::copySidProcedureProperties(route.getFlightplan().getProperties(),
                                                    flightplan.getProperties());
      }
      else if(insertBefore >= route.size() - 1)
      {
        // Insert before destination ==============
        beforeDestInsert = true;

        // Remove all arrival legs and properties
        route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

        // Correct insert position and start of selection for removed arrival legs
        insertBefore = route.size() - 1;
        insertPosSelection = insertBefore;

        // No procedures taken from the loaded plan
      }
      else
      {
        // Insert in the middle ==============
        middleInsert = true;

        // No airway at start leg
        eraseAirway(insertBefore);

        // No procedures taken from the loaded plan
      }

      // Copy new legs to the flightplan object only - update route later
      for(auto it = flightplan.getEntries().rbegin(); it != flightplan.getEntries().rend(); ++it)
        route.getFlightplan().getEntries().insert(insertBefore, *it);
    }

    // Clear procedure legs from the flightplan object
    route.getFlightplan().removeNoSaveEntries();

    // Clear procedure structures
    route.clearProcedures(proc::PROCEDURE_ALL);

    // Clear procedure legs from route object only since indexes are not consistent now
    route.clearProcedureLegs(proc::PROCEDURE_ALL, true /* clear route */, false /* clear flight plan */);

    // Rebuild all legs from flightplan object and properties
    route.createRouteLegsFromFlightplan();

    // Load procedures and add legs
    loadProceduresFromFlightplan(true /* clear old procedure properties */, false /* quiet */);
    route.updateAll();
    route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

    route.updateActiveLegAndPos(true /* force update */);
    updateTableModel();

    postChange(undoCommand);
    NavApp::updateWindowTitle();

    // Select newly imported flight plan legs
    if(afterDestAppend)
      selectRange(insertPosSelection, route.size() - 1);
    else if(beforeDepartPrepend)
      selectRange(0, flightplan.getEntries().size() + route.getStartIndexAfterProcedure() - 1); // fix
    else if(beforeDestInsert)
      selectRange(insertPosSelection, route.size() - 2);
    else if(middleInsert)
      selectRange(insertPosSelection, insertPosSelection + flightplan.getEntries().size() - 1);

    updateErrorLabel();

    emit routeChanged(true);
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteController::saveFlighplanAs(const QString& filename, pln::FileFormat targetFileFormat)
{
  qDebug() << Q_FUNC_INFO << filename << targetFileFormat;
  routeFilename = filename;
  routeFileFormat = targetFileFormat;
  route.getFlightplan().setFileFormat(targetFileFormat);
  return saveFlightplan(false);
}

/* Save flight plan using the same format indicated in the flight plan object */
bool RouteController::saveFlightplan(bool cleanExport)
{
  // Get a copy that has procedures replaced with waypoints depending on settings
  // Also fills altitude in flight plan entry position
  Flightplan flightplan = RouteExport::routeAdjustedToProcedureOptions(route).getFlightplan();
  qDebug() << Q_FUNC_INFO << "flightplan.getFileFormat()" << flightplan.getFileFormat()
           << "routeFileFormat" << routeFileFormat;

  try
  {
    if(!cleanExport)
    {
      fileDeparture = flightplan.getDepartureIdent();
      fileDestination = flightplan.getDestinationIdent();
      fileIfrVfr = flightplan.getFlightplanType();
    }

    // Will throw an exception if something goes wrong

    // Remember altitude in local units and set to feet before saving
    int oldCruise = flightplan.getCruisingAltitude();
    flightplan.setCruisingAltitude(
      atools::roundToInt(Unit::rev(static_cast<float>(flightplan.getCruisingAltitude()), Unit::altFeetF)));

    QHash<QString, QString>& properties = flightplan.getProperties();

    assignAircraftPerformance(flightplan);
    properties.insert(pln::SIMDATA, NavApp::getDatabaseMetaSim()->getDataSource());
    properties.insert(pln::NAVDATA, NavApp::getDatabaseMetaNav()->getDataSource());
    properties.insert(pln::AIRAC_CYCLE, NavApp::getDatabaseAiracCycleNav());

    atools::fs::pln::SaveOptions options = atools::fs::pln::SAVE_NO_OPTIONS;

    if(OptionData::instance().getFlags() & opts::ROUTE_GARMIN_USER_WPT)
      options |= atools::fs::pln::SAVE_GNS_USER_WAYPOINTS;

    if(cleanExport)
      options |= atools::fs::pln::SAVE_CLEAN;

    // Check for a circle-to-land approach without runway - add a random (best) runway from the airport to
    // satisfy the X-Plane GPS/FMC/G1000
    bool dummyRwAdded = false;
    if(route.last().getAirport().isValid() &&
       flightplan.getFileFormat() == atools::fs::pln::FMS11 &&
       flightplan.getProperties().value(atools::fs::pln::APPROACHRW).isEmpty() &&
       (!flightplan.getProperties().value(atools::fs::pln::APPROACH).isEmpty() ||
        !flightplan.getProperties().value(atools::fs::pln::APPROACH_ARINC).isEmpty()))
    {
      // Get best runway - longest with probably hard surface
      const QList<map::MapRunway> *runways = airportQuery->getRunways(route.last().getId());
      if(runways != nullptr && !runways->isEmpty())
      {
        dummyRwAdded = true;
        flightplan.getProperties().insert(atools::fs::pln::APPROACHRW, runways->last().primaryName);
      }
    }

    // Save PLN, FLP or FMS
    flightplanIO->save(flightplan, routeFilename, NavApp::getDatabaseAiracCycleNav(), options);

    if(dummyRwAdded)
      flightplan.getProperties().insert(atools::fs::pln::APPROACHRW, QString());

    if(flightplan.getFileFormat() == atools::fs::pln::PLN_FS9 || flightplan.getFileFormat() == atools::fs::pln::PLN_FSC)
    {
      // Old format is always saved as new after question dialog
      flightplan.setFileFormat(atools::fs::pln::PLN_FSX);
      routeFileFormat = atools::fs::pln::PLN_FSX;
    }

    flightplan.setCruisingAltitude(oldCruise);

    if(!cleanExport)
    {
      // Set clean undo state index since QUndoStack only returns weird values
      undoIndexClean = undoIndex;
      undoStack->setClean();
      NavApp::updateWindowTitle();
      qDebug() << "saveFlightplan undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
    }
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteController::exportFlighplanAsClean(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;

  QString savedFilename = routeFilename;
  atools::fs::pln::FileFormat savedFileFormat = routeFileFormat;

  routeFilename = filename;
  routeFileFormat = atools::fs::pln::PLN_FSX;
  bool retval = saveFlightplan(true /* clean export */);

  // Revert back to original name
  routeFilename = savedFilename;
  routeFileFormat = savedFileFormat;
  return retval;
}

void RouteController::calculateDirect()
{
  qDebug() << Q_FUNC_INFO;

  // Stop any background tasks
  beforeRouteCalc();

  RouteCommand *undoCommand = preChange(tr("Direct Calculation"));

  route.getFlightplan().setRouteType(atools::fs::pln::DIRECT);
  route.removeRouteLegs();

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

  updateTableModel();
  postChange(undoCommand);
  NavApp::updateWindowTitle();
  updateErrorLabel();
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Calculated direct flight plan."));
}

void RouteController::beforeRouteCalc()
{
  routeAltDelayTimer.stop();
  emit preRouteCalc();
}

void RouteController::calculateRadionav(int fromIndex, int toIndex)
{
  qDebug() << Q_FUNC_INFO;
  // Changing mode might need a clear
  routeNetworkRadio->setMode(nw::ROUTE_RADIONAV);

  RouteFinder routeFinder(routeNetworkRadio);

  if(calculateRouteInternal(&routeFinder, atools::fs::pln::VOR, tr("Radionnav Flight Plan Calculation"),
                            false /* fetch airways */, false /* Use altitude */,
                            fromIndex, toIndex))
    NavApp::setStatusMessage(tr("Calculated radio navaid flight plan."));
  else
    NavApp::setStatusMessage(tr("No route found."));
}

void RouteController::calculateRadionav()
{
  calculateRadionav(-1, -1);
}

void RouteController::calculateHighAlt(int fromIndex, int toIndex)
{
  qDebug() << Q_FUNC_INFO;
  routeNetworkAirway->setMode(nw::ROUTE_JET);

  RouteFinder routeFinder(routeNetworkAirway);

  if(calculateRouteInternal(&routeFinder, atools::fs::pln::HIGH_ALTITUDE,
                            tr("High altitude Flight Plan Calculation"),
                            true /* fetch airways */, false /* Use altitude */,
                            fromIndex, toIndex))
    NavApp::setStatusMessage(tr("Calculated high altitude (Jet airways) flight plan."));
  else
    NavApp::setStatusMessage(tr("No route found."));
}

void RouteController::calculateHighAlt()
{
  calculateHighAlt(-1, -1);
}

void RouteController::calculateLowAlt(int fromIndex, int toIndex)
{
  qDebug() << Q_FUNC_INFO;
  routeNetworkAirway->setMode(nw::ROUTE_VICTOR);

  RouteFinder routeFinder(routeNetworkAirway);

  if(calculateRouteInternal(&routeFinder, atools::fs::pln::LOW_ALTITUDE,
                            tr("Low altitude Flight Plan Calculation"),
                            true /* fetch airways */, false /* Use altitude */,
                            fromIndex, toIndex))
    NavApp::setStatusMessage(tr("Calculated low altitude (Victor airways) flight plan."));
  else
    NavApp::setStatusMessage(tr("No route found."));
}

void RouteController::calculateLowAlt()
{
  calculateLowAlt(-1, -1);
}

void RouteController::calculateSetAlt(int fromIndex, int toIndex)
{
  qDebug() << Q_FUNC_INFO;
  routeNetworkAirway->setMode(nw::ROUTE_VICTOR | nw::ROUTE_JET);

  RouteFinder routeFinder(routeNetworkAirway);

  // Just decide by given altiude if this is a high or low plan
  atools::fs::pln::RouteType type;
  if(route.getFlightplan().getCruisingAltitude() >= Unit::altFeetF(20000.f))
    type = atools::fs::pln::HIGH_ALTITUDE;
  else
    type = atools::fs::pln::LOW_ALTITUDE;

  if(calculateRouteInternal(&routeFinder, type, tr("Low altitude flight plan"),
                            true /* fetch airways */, true /* Use altitude */,
                            fromIndex, toIndex))
    NavApp::setStatusMessage(tr("Calculated high/low flight plan for given altitude."));
  else
    NavApp::setStatusMessage(tr("No route found."));
}

void RouteController::calculateSetAlt()
{
  calculateSetAlt(-1, -1);
}

/* Calculate a flight plan to all types */
bool RouteController::calculateRouteInternal(RouteFinder *routeFinder, atools::fs::pln::RouteType type,
                                             const QString& commandName, bool fetchAirways,
                                             bool useSetAltitude, int fromIndex, int toIndex)
{
  bool calcRange = fromIndex != -1 && toIndex != -1;

  // Create wait cursor if calculation takes too long
  QGuiApplication::setOverrideCursor(Qt::WaitCursor);

  // Stop any background tasks
  beforeRouteCalc();

  Flightplan& flightplan = route.getFlightplan();

  int cruiseFt = atools::roundToInt(Unit::rev(flightplan.getCruisingAltitude(), Unit::altFeetF));
  int altitude = useSetAltitude ? cruiseFt : 0;

  routeFinder->setPreferVorToAirway(OptionData::instance().getFlags() & opts::ROUTE_PREFER_VOR);
  routeFinder->setPreferNdbToAirway(OptionData::instance().getFlags() & opts::ROUTE_PREFER_NDB);

  Pos departurePos, destinationPos;

  if(calcRange)
  {
    fromIndex = std::max(route.getStartIndexAfterProcedure(), fromIndex);
    toIndex = std::min(route.getDestinationIndexBeforeProcedure(), toIndex);

    departurePos = route.at(fromIndex).getPosition();
    destinationPos = route.at(toIndex).getPosition();
  }
  else
  {
    departurePos = route.getStartAfterProcedure().getPosition();
    destinationPos = route.getDestinationBeforeProcedure().getPosition();
  }

  // Calculate the route
  bool found = routeFinder->calculateRoute(departurePos, destinationPos, altitude);

  if(found)
  {
    // A route was found
    float distance = 0.f;
    QVector<rf::RouteEntry> calculatedRoute;

    // Fetch waypoints
    routeFinder->extractRoute(calculatedRoute, distance);

    // Compare to direct connection and check if route is too long
    float directDistance = departurePos.distanceMeterTo(destinationPos);
    float ratio = distance / directDistance;
    qDebug() << "route distance" << QString::number(distance, 'f', 0)
             << "direct distance" << QString::number(directDistance, 'f', 0) << "ratio" << ratio;

    if(ratio < MAX_DISTANCE_DIRECT_RATIO)
    {
      // Start undo
      RouteCommand *undoCommand = preChange(commandName);

      QList<FlightplanEntry>& entries = flightplan.getEntries();

      flightplan.setRouteType(type);
      if(calcRange)
        entries.erase(flightplan.getEntries().begin() + fromIndex + 1, flightplan.getEntries().begin() + toIndex);
      else
        // Erase all but start and destination
        entries.erase(flightplan.getEntries().begin() + 1, entries.end() - 1);

      int idx = 1;
      // Create flight plan entries - will be copied later to the route map objects
      for(const rf::RouteEntry& routeEntry : calculatedRoute)
      {
        FlightplanEntry flightplanEntry;
        entryBuilder->buildFlightplanEntry(routeEntry.ref.id, atools::geo::EMPTY_POS, routeEntry.ref.type,
                                           flightplanEntry, fetchAirways);
        if(fetchAirways && routeEntry.airwayId != -1)
          // Get airway by id - needed to fetch the name first
          updateFlightplanEntryAirway(routeEntry.airwayId, flightplanEntry);

        if(calcRange)
          entries.insert(flightplan.getEntries().begin() + fromIndex + idx, flightplanEntry);
        else
          entries.insert(entries.end() - 1, flightplanEntry);
        idx++;
      }

      // Remove procedure points from flight plan
      flightplan.removeNoSaveEntries();

      // Copy flight plan to route object
      route.createRouteLegsFromFlightplan();

      // Reload procedures from properties
      loadProceduresFromFlightplan(true /* clear old procedure properties */, true /* quiet */);
      QGuiApplication::restoreOverrideCursor();

      // Remove duplicates in flight plan and route
      route.removeDuplicateRouteLegs();
      route.updateAll();

      bool adjustRouteType = type != atools::fs::pln::HIGH_ALTITUDE && type != atools::fs::pln::LOW_ALTITUDE &&
                             type != atools::fs::pln::VOR;
      route.updateAirwaysAndAltitude(!useSetAltitude /* adjustRouteAltitude */, adjustRouteType);

      route.updateActiveLegAndPos(true /* force update */);

      // Need to update again after updateAll and altitude change
      route.updateLegAltitudes();

      updateTableModel();

      postChange(undoCommand);
      NavApp::updateWindowTitle();

#ifdef DEBUG_INFORMATION
      qDebug() << flightplan;
#endif

      updateErrorLabel();
      emit routeChanged(true);
    }
    else
      // Too long
      found = false;
  }

  QGuiApplication::restoreOverrideCursor();
  if(!found)
    atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOWROUTE_ERROR,
                                                   tr("Cannot find a route.\n"
                                                      "Try another routing type or create the flight plan manually."),
                                                   tr("Do not &show this dialog again."));
#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << route;
#endif

  return found;
}

void RouteController::adjustFlightplanAltitude()
{
  qDebug() << Q_FUNC_INFO;

  if(route.isEmpty())
    return;

  Flightplan& fp = route.getFlightplan();
  int alt = route.adjustAltitude(fp.getCruisingAltitude());

  if(alt != fp.getCruisingAltitude())
  {
    RouteCommand *undoCommand = nullptr;

    // if(route.getFlightplan().canSaveAltitude())
    undoCommand = preChange(tr("Adjust altitude"), rctype::ALTITUDE);
    fp.setCruisingAltitude(alt);

    updateTableModel();

    // Need to update again after updateAll and altitude change
    route.updateLegAltitudes();

    postChange(undoCommand);

    NavApp::updateWindowTitle();
    updateErrorLabel();

    if(!route.isEmpty())
      emit routeAltitudeChanged(route.getCruisingAltitudeFeet());

    NavApp::setStatusMessage(tr("Adjusted flight plan altitude."));
  }
}

void RouteController::reverseRoute()
{
  qDebug() << Q_FUNC_INFO;

  RouteCommand *undoCommand = preChange(tr("Reverse"), rctype::REVERSE);

  // Remove all procedures and properties
  route.removeProcedureLegs(proc::PROCEDURE_ALL);

  route.getFlightplan().reverse();

  QList<FlightplanEntry>& entries = route.getFlightplan().getEntries();
  if(entries.size() > 3)
  {
    // Move all airway names one entry down
    for(int i = entries.size() - 2; i >= 1; i--)
      entries[i].setAirway(entries.at(i - 1).getAirway());
  }

  route.createRouteLegsFromFlightplan();
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);
  updateStartPositionBestRunway(true /* force */, false /* undo */);

  route.updateActiveLegAndPos(true /* force update */);
  updateTableModel();

  postChange(undoCommand);
  NavApp::updateWindowTitle();
  updateErrorLabel();
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Reversed flight plan."));
}

void RouteController::preDatabaseLoad()
{
  routeNetworkRadio->deInitQueries();
  routeNetworkAirway->deInitQueries();
  routeAltDelayTimer.stop();
}

void RouteController::postDatabaseLoad()
{
  routeNetworkRadio->initQueries();
  routeNetworkAirway->initQueries();

  // Remove the legs but keep the properties
  route.clearProcedures(proc::PROCEDURE_ALL);
  route.clearProcedureLegs(proc::PROCEDURE_ALL);

  route.createRouteLegsFromFlightplan();
  loadProceduresFromFlightplan(false /* clear old procedure properties */, false /* quiet */);
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

  // Update runway or parking if one of these has changed due to the database switch
  Flightplan& flightplan = route.getFlightplan();
  if(!flightplan.getEntries().isEmpty() &&
     flightplan.getEntries().first().getWaypointType() == atools::fs::pln::entry::AIRPORT &&
     flightplan.getDepartureParkingName().isEmpty())
    updateStartPositionBestRunway(false, true);

  route.updateActiveLegAndPos(true /* force update */);
  updateTableModel();
  updateErrorLabel();
  NavApp::updateWindowTitle();
  routeAltDelayTimer.start(ROUTE_ALT_CHANGE_DELAY_MS);
}

/* Double click into table view */
void RouteController::doubleClick(const QModelIndex& index)
{
  qDebug() << Q_FUNC_INFO;
  if(index.isValid())
  {
    qDebug() << "mouseDoubleClickEvent";

    const RouteLeg& mo = route.at(index.row());

    if(mo.getMapObjectType() == map::AIRPORT)
      emit showRect(mo.getAirport().bounding, true);
    else
      emit showPos(mo.getPosition(), 0.f, true);

    map::MapSearchResult result;
    mapQuery->getMapObjectById(result, mo.getMapObjectType(), mo.getId(), false /* airport from nav database */);
    emit showInformation(result);
  }
}

void RouteController::updateMoveAndDeleteActions()
{
  QItemSelectionModel *sm = view->selectionModel();

  if(view->selectionModel() == nullptr)
    return;

  if(sm->hasSelection() && model->rowCount() > 0)
  {
    bool containsProc = false, moveDownTouchesProc = false, moveUpTouchesProc = false;
    QList<int> rows;
    selectedRows(rows, false);
    for(int row : rows)
    {
      if(route.at(row).isAnyProcedure())
      {
        containsProc = true;
        break;
      }
    }
    moveUpTouchesProc = rows.first() > 0 && route.at(rows.first() - 1).isAnyProcedure();
    moveDownTouchesProc = rows.first() < route.size() - 1 && route.at(rows.first() + 1).isAnyProcedure();

    Ui::MainWindow *ui = NavApp::getMainUi();
    ui->actionRouteLegUp->setEnabled(false);
    ui->actionRouteLegDown->setEnabled(false);
    ui->actionRouteDeleteLeg->setEnabled(false);

    if(model->rowCount() > 1)
    {
      ui->actionRouteDeleteLeg->setEnabled(true);
      ui->actionRouteLegUp->setEnabled(sm->hasSelection() && !sm->isRowSelected(0, QModelIndex()) &&
                                       !containsProc && !moveUpTouchesProc);

      ui->actionRouteLegDown->setEnabled(sm->hasSelection() &&
                                         !sm->isRowSelected(model->rowCount() - 1, QModelIndex()) &&
                                         !containsProc && !moveDownTouchesProc);
    }
    else if(model->rowCount() == 1)
      // Only one waypoint - nothing to move
      ui->actionRouteDeleteLeg->setEnabled(true);
  }
}

/* From context menu */
void RouteController::showInformationMenu()
{
  qDebug() << Q_FUNC_INFO;
  QModelIndex index = view->currentIndex();
  if(index.isValid())
  {
    const RouteLeg& routeLeg = route.at(index.row());
    map::MapSearchResult result;
    mapQuery->getMapObjectById(result, routeLeg.getMapObjectType(), routeLeg.getId(),
                               false /* airport from nav database */);
    emit showInformation(result);
  }
}

/* From context menu */
void RouteController::showProceduresMenu()
{
  QModelIndex index = view->currentIndex();
  if(index.isValid())
  {
    const RouteLeg& routeLeg = route.at(index.row());
    emit showProcedures(routeLeg.getAirport());
  }
}

/* From context menu */
void RouteController::showOnMapMenu()
{
  QModelIndex index = view->currentIndex();
  if(index.isValid())
  {
    const RouteLeg& routeLeg = route.at(index.row());

    if(routeLeg.getMapObjectType() == map::AIRPORT)
      emit showRect(routeLeg.getAirport().bounding, false);
    else
      emit showPos(routeLeg.getPosition(), 0.f, false);

    if(routeLeg.getMapObjectType() == map::AIRPORT)
      NavApp::setStatusMessage(tr("Showing airport on map."));
    else
      NavApp::setStatusMessage(tr("Showing navaid on map."));
  }
}

void RouteController::activateLegTriggered()
{
  QList<int> rows;
  selectedRows(rows, false);

  if(!rows.isEmpty())
    activateLegManually(rows.first());
}

void RouteController::helpClicked()
{
  atools::gui::HelpHandler::openHelpUrlWeb(mainWindow, lnm::helpOnlineUrl + "FLIGHTPLAN.html",
                                           lnm::helpLanguageOnline());
}

void RouteController::selectAllTriggered()
{
  view->selectAll();
}

void RouteController::tableContextMenu(const QPoint& pos)
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  QPoint menuPos = QCursor::pos();
  // Use widget center if position is not inside widget
  if(!ui->tableViewRoute->rect().contains(ui->tableViewRoute->mapFromGlobal(QCursor::pos())))
    menuPos = ui->tableViewRoute->mapToGlobal(ui->tableViewRoute->rect().center());

  // Move menu position off the cursor to avoid accidental selection on touchpads
  menuPos += QPoint(3, 3);

  qDebug() << "tableContextMenu";

  // Save text which will be changed below
  atools::gui::ActionTextSaver saver({ui->actionMapNavaidRange, ui->actionMapEditUserWaypoint,
                                      ui->actionRouteShowApproaches, ui->actionRouteDeleteLeg,
                                      ui->actionRouteInsert, ui->actionMapTrafficPattern});
  Q_UNUSED(saver);

  // Re-enable actions on exit to allow keystrokes
  atools::gui::ActionStateSaver stateSaver(
  {
    ui->actionRouteShowInformation, ui->actionRouteShowApproaches, ui->actionRouteShowOnMap,
    ui->actionRouteActivateLeg, ui->actionRouteLegUp, ui->actionRouteLegDown, ui->actionRouteDeleteLeg,
    ui->actionMapEditUserWaypoint,
    ui->actionRouteCalcRadionavSelected, ui->actionRouteCalcHighAltSelected, ui->actionRouteCalcLowAltSelected,
    ui->actionRouteCalcSetAltSelected,
    ui->actionMapRangeRings, ui->actionMapTrafficPattern, ui->actionMapNavaidRange,
    ui->actionRouteTableCopy, ui->actionRouteTableSelectNothing, ui->actionRouteTableSelectAll,
    ui->actionRouteResetView, ui->actionRouteSetMark, ui->actionRouteInsert, ui->actionRouteTableAppend
  });
  Q_UNUSED(stateSaver);

  QModelIndex index = view->indexAt(pos);
  const RouteLeg *routeLeg = nullptr, *prevRouteLeg = nullptr;
  int row = -1;
  if(index.isValid())
  {
    row = index.row();
    routeLeg = &route.at(row);
    if(index.row() > 0)
      prevRouteLeg = &route.at(row - 1);
  }

  QMenu calcMenu(tr("Calculate for &selected legs"));
  QMenu menu;

  updateMoveAndDeleteActions();

  ui->actionRouteTableCopy->setEnabled(index.isValid());

  bool insert = false;

  // Menu above a row
  if(routeLeg != nullptr)
  {
    ui->actionRouteShowInformation->setEnabled(routeLeg->isValid() &&
                                               routeLeg->isRoute() &&
                                               routeLeg->getMapObjectType() != map::USERPOINTROUTE &&
                                               routeLeg->getMapObjectType() != map::INVALID);

    if(routeLeg->isValid())
    {
      if(prevRouteLeg == nullptr)
        // allow to insert before first one
        insert = true;
      else if(prevRouteLeg->isRoute() && routeLeg->isAnyProcedure() &&
              routeLeg->getProcedureType() & proc::PROCEDURE_ARRIVAL_ALL)
        // Insert between enroute waypoint and approach or STAR
        insert = true;
      else if(routeLeg->isRoute() && prevRouteLeg->isAnyProcedure() &&
              prevRouteLeg->getProcedureType() & proc::PROCEDURE_DEPARTURE)
        // Insert between SID and waypoint
        insert = true;
      else
        insert = routeLeg->isRoute();
    }

    ui->actionRouteShowApproaches->setEnabled(false);
    if(routeLeg->isValid() && routeLeg->getMapObjectType() == map::AIRPORT)
    {
      bool hasDeparture = NavApp::getAirportQueryNav()->hasDepartureProcedures(routeLeg->getIdent());
      bool hasAnyArrival = NavApp::getAirportQueryNav()->hasAnyArrivalProcedures(routeLeg->getIdent());

      if(hasAnyArrival || hasDeparture)
      {
        bool airportDeparture = NavApp::getRouteConst().isAirportDeparture(routeLeg->getIdent());
        bool airportDestination = NavApp::getRouteConst().isAirportDestination(routeLeg->getIdent());
        if(airportDeparture)
        {
          if(hasDeparture)
          {
            ui->actionRouteShowApproaches->setEnabled(true);
            ui->actionRouteShowApproaches->setText(ui->actionRouteShowApproaches->text().arg(tr("Departure ")));
          }
          else
            ui->actionRouteShowApproaches->setText(tr("Show procedures (airport has no departure procedure)"));
        }
        else if(airportDestination)
        {
          if(hasAnyArrival)
          {
            ui->actionRouteShowApproaches->setEnabled(true);
            ui->actionRouteShowApproaches->setText(ui->actionRouteShowApproaches->text().arg(tr("Arrival ")));
          }
          else
            ui->actionRouteShowApproaches->setText(tr("Show procedures (airport has no arrival procedure)"));
        }
        else
        {
          ui->actionRouteShowApproaches->setEnabled(true);
          ui->actionRouteShowApproaches->setText(ui->actionRouteShowApproaches->text().arg(tr("all ")));
        }
      }
      else
        ui->actionRouteShowApproaches->setText(tr("Show procedures (airport has no procedure)"));
    }
    else
      ui->actionRouteShowApproaches->setText(tr("Show procedures"));

    ui->actionRouteShowOnMap->setEnabled(true);
    ui->actionMapRangeRings->setEnabled(true);
    ui->actionRouteSetMark->setEnabled(true);

    // ui->actionRouteDeleteLeg->setText(route->isAnyProcedure() ?
    // tr("Delete Procedure") : tr("Delete selected Legs"));

#ifdef DEBUG_MOVING_AIRPLANE
    ui->actionRouteActivateLeg->setEnabled(routeLeg->isValid());
#else
    ui->actionRouteActivateLeg->setEnabled(routeLeg->isValid() && NavApp::isConnected());
#endif
  }
  else
  {
    ui->actionRouteShowInformation->setEnabled(false);
    ui->actionRouteShowApproaches->setEnabled(false);
    ui->actionRouteShowApproaches->setText(tr("Show procedures"));
    ui->actionRouteActivateLeg->setEnabled(false);
    ui->actionRouteShowOnMap->setEnabled(false);
    ui->actionMapRangeRings->setEnabled(false);
    ui->actionRouteSetMark->setEnabled(false);
  }

  ui->actionRouteTableAppend->setEnabled(!route.isEmpty());
  if(insert)
  {
    ui->actionRouteInsert->setEnabled(true);
    ui->actionRouteInsert->setText(ui->actionRouteInsert->text().arg(routeLeg->getIdent()));
  }
  else
  {
    ui->actionRouteInsert->setEnabled(false);
    ui->actionRouteInsert->setText(tr("Insert Flight Plan before ..."));
  }

  if(routeLeg != nullptr && routeLeg->getAirport().isValid() && !routeLeg->getAirport().noRunways())
    ui->actionMapTrafficPattern->setEnabled(true);
  else
    ui->actionMapTrafficPattern->setEnabled(false);
  ui->actionMapTrafficPattern->setText(tr("Display Airport Traffic Pattern"));

  // Get selected rows in ascending order
  QList<int> rows;
  selectedRows(rows, false);

  // Check if selected rows contain a procedure or if a procedure is between first and last selection
  bool containsProc = false;
  if(!rows.isEmpty())
    containsProc = route.at(rows.first()).isAnyProcedure() || route.at(rows.last()).isAnyProcedure();

  bool enableCalc = routeLeg != nullptr && rows.size() > 1 && !containsProc;

  calcMenu.setEnabled(enableCalc);
  ui->actionRouteCalcRadionavSelected->setEnabled(enableCalc);
  ui->actionRouteCalcHighAltSelected->setEnabled(enableCalc);
  ui->actionRouteCalcLowAltSelected->setEnabled(enableCalc);
  ui->actionRouteCalcSetAltSelected->setEnabled(enableCalc);

  ui->actionMapNavaidRange->setEnabled(false);

  ui->actionRouteTableSelectNothing->setEnabled(
    view->selectionModel() == nullptr ? false : view->selectionModel()->hasSelection());
  ui->actionRouteTableSelectAll->setEnabled(!route.isEmpty());

  ui->actionMapNavaidRange->setText(tr("Show Navaid Range"));

  ui->actionMapEditUserWaypoint->setEnabled(routeLeg != nullptr &&
                                            routeLeg->getMapObjectType() == map::USERPOINTROUTE);
  ui->actionMapEditUserWaypoint->setText(tr("Edit Position"));

  QList<int> selectedRouteLegIndexes;
  getSelectedRouteLegs(selectedRouteLegIndexes);
  // If there are any radio navaids in the selected list enable range menu item
  for(int idx : selectedRouteLegIndexes)
  {
    const RouteLeg& leg = route.at(idx);
    if(leg.getVor().isValid() || leg.getNdb().isValid())
    {
      ui->actionMapNavaidRange->setEnabled(true);
      break;
    }
  }

  menu.addAction(ui->actionRouteShowInformation);
  menu.addAction(ui->actionRouteShowApproaches);
  menu.addAction(ui->actionRouteShowOnMap);
  menu.addAction(ui->actionRouteActivateLeg);
  menu.addSeparator();

  menu.addAction(ui->actionRouteFollowSelection);
  menu.addSeparator();

  menu.addAction(ui->actionRouteLegUp);
  menu.addAction(ui->actionRouteLegDown);
  menu.addAction(ui->actionRouteDeleteLeg);
  menu.addAction(ui->actionMapEditUserWaypoint);
  menu.addSeparator();

  menu.addAction(ui->actionRouteInsert);
  menu.addAction(ui->actionRouteTableAppend);
  menu.addSeparator();

  calcMenu.addAction(ui->actionRouteCalcRadionavSelected);
  calcMenu.addAction(ui->actionRouteCalcHighAltSelected);
  calcMenu.addAction(ui->actionRouteCalcLowAltSelected);
  calcMenu.addAction(ui->actionRouteCalcSetAltSelected);
  menu.addMenu(&calcMenu);
  menu.addSeparator();

  menu.addAction(ui->actionMapRangeRings);
  menu.addAction(ui->actionMapNavaidRange);
  menu.addSeparator();
  menu.addAction(ui->actionMapTrafficPattern);
  menu.addSeparator();

  menu.addAction(ui->actionRouteTableCopy);
  menu.addAction(ui->actionRouteTableSelectAll);
  menu.addAction(ui->actionRouteTableSelectNothing);
  menu.addSeparator();

  menu.addAction(ui->actionRouteResetView);
  menu.addSeparator();

  menu.addAction(ui->actionRouteSetMark);

  QAction *action = menu.exec(menuPos);
  if(action != nullptr)
    qDebug() << Q_FUNC_INFO << "selected" << action->text();
  else
    qDebug() << Q_FUNC_INFO << "no action selected";

  if(action != nullptr)
  {
    if(action == ui->actionRouteResetView)
    {
      // Reorder columns to match model order
      QHeaderView *header = view->horizontalHeader();
      for(int i = 0; i < header->count(); i++)
        header->moveSection(header->visualIndex(i), i);

      view->resizeColumnsToContents();
      NavApp::setStatusMessage(tr("Table view reset to defaults."));
    }
    else if(action == ui->actionRouteSetMark && routeLeg != nullptr)
      emit changeMark(routeLeg->getPosition());
    else if(action == ui->actionMapRangeRings && routeLeg != nullptr)
      NavApp::getMapWidget()->addRangeRing(routeLeg->getPosition());
    else if(action == ui->actionMapTrafficPattern && routeLeg != nullptr)
      NavApp::getMapWidget()->addTrafficPattern(routeLeg->getAirport());
    else if(action == ui->actionMapNavaidRange)
    {
      // Show range rings for all radio navaids
      for(int idx : selectedRouteLegIndexes)
      {
        const RouteLeg& routeLegSel = route.at(idx);
        if(routeLegSel.getNdb().isValid() || routeLegSel.getVor().isValid())
        {
          map::MapObjectTypes type = routeLegSel.getMapObjectType();
          if(routeLegSel.isAnyProcedure())
          {
            if(routeLegSel.getNdb().isValid())
              type = map::NDB;
            if(routeLegSel.getVor().isValid())
              type = map::VOR;
          }
          NavApp::getMapWidget()->addNavRangeRing(routeLegSel.getPosition(), type,
                                                  routeLegSel.getIdent(), routeLegSel.getFrequencyOrChannel(),
                                                  routeLegSel.getRange());
        }
      }
    }
    // else if(action == ui->actionMapHideRangeRings)
    // NavApp::getMapWidget()->clearRangeRingsAndDistanceMarkers(); // Connected directly
    else if(action == ui->actionMapEditUserWaypoint)
      editUserWaypointName(index.row());
    // else if(action == ui->actionRouteTableAppend) // Done by signal from action
    // emit routeAppend();
    else if(action == ui->actionRouteInsert)
      emit routeInsert(row);
    else if(action == ui->actionRouteActivateLeg)
      activateLegManually(index.row());
    else if(action == ui->actionRouteCalcRadionavSelected)
      calculateRadionav(rows.first(), rows.last());
    else if(action == ui->actionRouteCalcHighAltSelected)
      calculateHighAlt(rows.first(), rows.last());
    else if(action == ui->actionRouteCalcLowAltSelected)
      calculateLowAlt(rows.first(), rows.last());
    else if(action == ui->actionRouteCalcSetAltSelected)
      calculateSetAlt(rows.first(), rows.last());
    // Other actions emit signals directly
  }
}

/* Activate leg manually from menu */
void RouteController::activateLegManually(int index)
{
  qDebug() << Q_FUNC_INFO << index;
  route.setActiveLeg(index);
  highlightNextWaypoint(route.getActiveLegIndex());
  // Use geometry changed flag to force redraw
  emit routeChanged(true);
}

void RouteController::clearSelection()
{
  view->clearSelection();
}

bool RouteController::hasSelection()
{
  return view->selectionModel() == nullptr ? false : view->selectionModel()->hasSelection();
}

void RouteController::editUserWaypointName(int index)
{
  qDebug() << Q_FUNC_INFO;

  UserWaypointDialog dialog(mainWindow, route.at(index).getIdent(), route.at(index).getPosition());
  if(dialog.exec() == QDialog::Accepted && !dialog.getName().isEmpty())
  {
    RouteCommand *undoCommand = nullptr;

    // if(route.getFlightplan().canSaveUserWaypointName())
    undoCommand = preChange(tr("Waypoint Name Change"));

    route[index].updateUserName(dialog.getName());
    route[index].updateUserPosition(dialog.getPos());

    model->item(index, rc::IDENT)->setText(dialog.getName());
    postChange(undoCommand);

    emit routeChanged(true);
  }
}

void RouteController::shownMapFeaturesChanged(map::MapObjectTypes types)
{
  // qDebug() << Q_FUNC_INFO;
  route.setShownMapFeatures(types);
  route.setShownMapFeatures(types);
}

/* Hide or show map hightlights if dock visibility changes */
void RouteController::dockVisibilityChanged(bool visible)
{
  Q_UNUSED(visible);

  // Visible - send update to show map highlights
  // Not visible - send update to hide highlights
  tableSelectionChanged(QItemSelection(), QItemSelection());
}

void RouteController::tableSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
  Q_UNUSED(selected);
  Q_UNUSED(deselected);

  updateMoveAndDeleteActions();
  QItemSelectionModel *sm = view->selectionModel();

  int selectedRows = 0;
  if(sm != nullptr && sm->hasSelection())
    selectedRows = sm->selectedRows().size();

#ifdef DEBUG_INFORMATION
  if(sm != nullptr && sm->hasSelection())
  {
    int r = sm->currentIndex().row();
    if(r != -1)
      qDebug() << r << "#" << route.at(r);
  }
#endif

  NavApp::getMainUi()->pushButtonRouteClearSelection->setEnabled(sm != nullptr && sm->hasSelection());

  emit routeSelectionChanged(selectedRows, model->rowCount());

  if(NavApp::getMainUi()->actionRouteFollowSelection->isChecked() &&
     sm != nullptr &&
     sm->currentIndex().isValid() &&
     sm->isSelected(sm->currentIndex()))
    emit showPos(route.at(sm->currentIndex().row()).getPosition(), map::INVALID_DISTANCE_VALUE, false);
}

/* Called by undo command */
void RouteController::changeRouteUndo(const atools::fs::pln::Flightplan& newFlightplan)
{
  // Keep our own index as a workaround
  undoIndex--;

  qDebug() << "changeRouteUndo undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
  changeRouteUndoRedo(newFlightplan);
}

/* Called by undo command */
void RouteController::changeRouteRedo(const atools::fs::pln::Flightplan& newFlightplan)
{
  // Keep our own index as a workaround
  undoIndex++;
  qDebug() << "changeRouteRedo undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
  changeRouteUndoRedo(newFlightplan);
}

/* Called by undo command when commands are merged */
void RouteController::undoMerge()
{
  undoIndex--;
  qDebug() << "undoMerge undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
}

/* Update window after undo or redo action */
void RouteController::changeRouteUndoRedo(const atools::fs::pln::Flightplan& newFlightplan)
{
  route.setFlightplan(newFlightplan);

  // Change format in plan according to last saved format
  route.getFlightplan().setFileFormat(routeFileFormat);

  route.createRouteLegsFromFlightplan();
  loadProceduresFromFlightplan(true /* clear old procedure properties */, true /* quiet */);
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

  updateTableModel();
  NavApp::updateWindowTitle();
  updateMoveAndDeleteActions();
  updateErrorLabel();
  emit routeChanged(true);
}

void RouteController::styleChanged()
{
  highlightProcedureItems();
  highlightNextWaypoint(route.getActiveLegIndexCorrected());
}

void RouteController::optionsChanged()
{
  zoomHandler->zoomPercent(OptionData::instance().getGuiRouteTableTextSize());
  updateIcons();
  updateTableHeaders();
  updateTableModel();

  updateUnits();
  view->update();
}

void RouteController::updateUnits()
{
  units->update();
}

bool RouteController::hasChanged() const
{
  return undoIndexClean == -1 || undoIndexClean != undoIndex;
}

bool RouteController::doesFilenameMatchRoute(atools::fs::pln::FileFormat format)
{
  if(!routeFilename.isEmpty())
  {
    if(!(OptionData::instance().getFlags() & opts::GUI_AVOID_OVERWRITE_FLIGHTPLAN))
      return true;

    if(format == atools::fs::pln::PLN_FS9 || format == atools::fs::pln::PLN_FSC || format == atools::fs::pln::PLN_FSX)
      return fileIfrVfr == route.getFlightplan().getFlightplanType() &&
             fileDeparture == route.getFlightplan().getDepartureIdent() &&
             fileDestination == route.getFlightplan().getDestinationIdent();
    else
      return fileDeparture == route.getFlightplan().getDepartureIdent() &&
             fileDestination == route.getFlightplan().getDestinationIdent();

  }
  return false;
}

/* Called by action */
void RouteController::moveSelectedLegsDown()
{
  qDebug() << "Leg down";
  moveSelectedLegsInternal(MOVE_DOWN);
}

/* Called by action */
void RouteController::moveSelectedLegsUp()
{
  qDebug() << "Leg up";
  moveSelectedLegsInternal(MOVE_UP);
}

void RouteController::moveSelectedLegsInternal(MoveDirection direction)
{
  // Get the selected rows. Depending on move direction order can be reversed to ease moving
  QList<int> rows;
  selectedRows(rows, direction == MOVE_DOWN /* reverse order */);

  if(!rows.isEmpty())
  {
    RouteCommand *undoCommand = preChange(tr("Move Waypoints"), rctype::MOVE);

    QModelIndex curIdx = view->currentIndex();
    // Remove selection
    if(view->selectionModel() != nullptr)
      view->selectionModel()->clear();
    for(int row : rows)
    {
      // Change flight plan
      route.getFlightplan().getEntries().move(row, row + direction);
      route.move(row, row + direction);

      // Move row
      model->insertRow(row + direction, model->takeRow(row));
    }

    int firstRow = rows.first();
    int lastRow = rows.last();

    bool forceDeparturePosition = false;
    if(direction == MOVE_DOWN)
    {
      qDebug() << "Move down" << firstRow << "to" << lastRow;
      // Departure moved down and was replaced by something else jumping up
      forceDeparturePosition = rows.contains(0);

      // Erase airway names at start of the moved block - last is smaller here
      eraseAirway(lastRow);
      eraseAirway(lastRow + 1);

      // Erase airway name at end of the moved block
      eraseAirway(firstRow + 2);
    }
    else if(direction == MOVE_UP)
    {
      qDebug() << "Move up" << firstRow << "to" << lastRow;
      // Something moved up and departure jumped up
      forceDeparturePosition = rows.contains(1);

      // Erase airway name at start of the moved block - last is larger here
      eraseAirway(firstRow - 1);

      // Erase airway names at end of the moved block
      eraseAirway(lastRow);
      eraseAirway(lastRow + 1);
    }

    route.updateAll();
    route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

    // Force update of start if departure airport was moved
    updateStartPositionBestRunway(forceDeparturePosition, false /* undo */);

    routeToFlightPlan();
    // Get type and cruise altitude from widgets
    updateFlightplanFromWidgets();

    route.updateActiveLegAndPos(true /* force update */);
    updateTableModel();

    // Restore current position at new moved position
    view->setCurrentIndex(model->index(curIdx.row() + direction, curIdx.column()));
    // Restore previous selection at new moved position
    selectList(rows, direction);

    updateMoveAndDeleteActions();

    postChange(undoCommand);
    NavApp::updateWindowTitle();
    updateErrorLabel();
    emit routeChanged(true);
    NavApp::setStatusMessage(tr("Moved flight plan legs."));
  }
}

void RouteController::eraseAirway(int row)
{
  if(0 <= row && row < route.getFlightplan().getEntries().size())
    route.getFlightplan()[row].setAirway(QString());
}

/* Called by action */
void RouteController::deleteSelectedLegs()
{
  qDebug() << Q_FUNC_INFO << "Leg delete";

  // Get selected rows
  QList<int> rows;
  selectedRows(rows, true /* reverse */);

  if(!rows.isEmpty())
  {
    proc::MapProcedureTypes procs = affectedProcedures(rows);

    // Do not merge for procedure deletes
    RouteCommand *undoCommand = preChange(
      procs & proc::PROCEDURE_ALL ? tr("Delete Procedure") : tr("Delete Waypoints"),
      procs & proc::PROCEDURE_ALL ? rctype::EDIT : rctype::DELETE);

    int firstRow = rows.last();

    if(view->selectionModel() != nullptr)
      view->selectionModel()->clear();
    for(int row : rows)
    {
      route.getFlightplan().getEntries().removeAt(row);

      eraseAirway(row);

      route.removeAt(row);
      model->removeRow(row);
    }

    route.removeProcedureLegs(procs);

    route.updateAll();
    route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

    // Force update of start if departure airport was removed
    updateStartPositionBestRunway(rows.contains(0) /* force */, false /* undo */);

    routeToFlightPlan();
    // Get type and cruise altitude from widgets
    updateFlightplanFromWidgets();

    route.updateActiveLegAndPos(true /* force update */);
    updateTableModel();

    // Update current position at the beginning of the former selection
    view->setCurrentIndex(model->index(firstRow, 0));
    updateMoveAndDeleteActions();

    postChange(undoCommand);
    NavApp::updateWindowTitle();
    updateErrorLabel();
    emit routeChanged(true);
    NavApp::setStatusMessage(tr("Removed flight plan legs."));
  }
}

/* Get selected row numbers from the table model */
void RouteController::selectedRows(QList<int>& rows, bool reverse)
{
  if(view->selectionModel() != nullptr)
  {
    QItemSelection sm = view->selectionModel()->selection();
    for(const QItemSelectionRange& rng : sm)
    {
      for(int row = rng.top(); row <= rng.bottom(); row++)
        rows.append(row);
    }
  }

  if(!rows.isEmpty())
  {
    // Remove from bottom to top - otherwise model creates a mess
    std::sort(rows.begin(), rows.end());
    if(reverse)
      std::reverse(rows.begin(), rows.end());
  }
}

/* Select all columns of the given rows adding offset to each row index */
void RouteController::selectList(const QList<int>& rows, int offset)
{
  QItemSelection newSel;

  for(int row : rows)
    // Need to select all columns
    newSel.append(QItemSelectionRange(model->index(row + offset, rc::FIRST_COLUMN),
                                      model->index(row + offset, rc::LAST_COLUMN)));

  view->selectionModel()->select(newSel, QItemSelectionModel::ClearAndSelect);
}

void RouteController::selectRange(int from, int to)
{
  QItemSelection newSel;

  int maxRows = view->model()->rowCount();

  if(from < 0 || to < 0 || from > maxRows - 1 || to > maxRows - 1)
    qWarning() << Q_FUNC_INFO << "not in range from" << from << "to" << to << ", min 0 max" << maxRows;

  from = std::min(std::max(from, 0), maxRows);
  to = std::min(std::max(to, 0), maxRows);

  newSel.append(QItemSelectionRange(model->index(from, rc::FIRST_COLUMN),
                                    model->index(to, rc::LAST_COLUMN)));

  view->selectionModel()->select(newSel, QItemSelectionModel::ClearAndSelect);
}

void RouteController::routeSetHelipad(const map::MapHelipad& helipad)
{
  qDebug() << Q_FUNC_INFO << helipad.id;

  map::MapStart start;
  airportQuery->getStartById(start, helipad.startId);

  routeSetStartPosition(start);
}

void RouteController::routeSetParking(const map::MapParking& parking)
{
  qDebug() << Q_FUNC_INFO << parking.id;

  RouteCommand *undoCommand = nullptr;

  // if(route.getFlightplan().canSaveDepartureParking())
  undoCommand = preChange(tr("Set Parking"));

  if(route.isEmpty() || route.first().getMapObjectType() != map::AIRPORT ||
     route.first().getId() != parking.airportId)
  {
    // No route, no start airport or different airport
    map::MapAirport ap;
    airportQuery->getAirportById(ap, parking.airportId);
    routeSetDepartureInternal(ap);
    route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);
  }

  // Update the current airport which is new or the same as the one used by the parking spot
  route.getFlightplan().setDepartureParkingName(map::parkingNameForFlightplan(parking));
  route.getFlightplan().setDeparturePosition(parking.position, route.first().getPosition().getAltitude());
  route.first().setDepartureParking(parking);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);
  routeToFlightPlan();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();
  updateTableModel();
  postChange(undoCommand);
  NavApp::updateWindowTitle();
  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Departure set to %1 parking %2.").arg(route.first().getIdent()).
                           arg(map::parkingNameNumberType(parking)));
}

/* Set start position (runway, helipad) for departure */
void RouteController::routeSetStartPosition(map::MapStart start)
{
  qDebug() << "route set start id" << start.id;

  RouteCommand *undoCommand = preChange(tr("Set Start Position"));

  if(route.isEmpty() || route.first().getMapObjectType() != map::AIRPORT ||
     route.first().getId() != start.airportId)
  {
    // No route, no start airport or different airport
    map::MapAirport ap;
    airportQuery->getAirportById(ap, start.airportId);
    routeSetDepartureInternal(ap);
    route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);
  }

  // No need to update airport since this is called from dialog only

  // Update the current airport which is new or the same as the one used by the parking spot
  // Use helipad number or runway name
  route.getFlightplan().setDepartureParkingName(start.runwayName);

  route.getFlightplan().setDeparturePosition(start.position, route.first().getPosition().getAltitude());
  route.first().setDepartureStart(start);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);
  routeToFlightPlan();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();
  updateTableModel();
  postChange(undoCommand);
  NavApp::updateWindowTitle();
  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Departure set to %1 start position %2.").arg(route.first().getIdent()).
                           arg(start.runwayName));
}

void RouteController::routeSetDeparture(map::MapAirport airport)
{
  qDebug() << Q_FUNC_INFO << airport.id << airport.ident;

  RouteCommand *undoCommand = preChange(tr("Set Departure"));

  routeSetDepartureInternal(airport);

  route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);
  routeToFlightPlan();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  route.updateActiveLegAndPos(true /* force update */);
  updateTableModel();

  postChange(undoCommand);
  NavApp::updateWindowTitle();
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Departure set to %1.").arg(route.first().getIdent()));
}

/* Add departure and add best runway start position */
void RouteController::routeSetDepartureInternal(const map::MapAirport& airport)
{
  FlightplanEntry entry;
  entryBuilder->buildFlightplanEntry(airport, entry);

  Flightplan& flightplan = route.getFlightplan();
  if(!flightplan.isEmpty())
  {
    const FlightplanEntry& first = flightplan.getEntries().first();
    if(first.getWaypointType() == pln::entry::AIRPORT &&
       flightplan.getDepartureIdent() == first.getIcaoIdent() && flightplan.getEntries().size() > 1)
    {
      flightplan.getEntries().removeAt(0);
      route.removeAt(0);
    }
  }

  flightplan.getEntries().prepend(entry);

  RouteLeg routeLeg(&flightplan);
  routeLeg.createFromAirport(0, airport, nullptr);
  route.prepend(routeLeg);

  updateStartPositionBestRunway(true /* force */, false /* undo */);
}

void RouteController::routeSetDestination(map::MapAirport airport)
{
  qDebug() << Q_FUNC_INFO << airport.id << airport.ident;

  RouteCommand *undoCommand = preChange(tr("Set Destination"));

  routeSetDestinationInternal(airport);

  route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);
  routeToFlightPlan();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  route.updateActiveLegAndPos(true /* force update */);
  updateTableModel();

  postChange(undoCommand);
  NavApp::updateWindowTitle();
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Destination set to %1.").arg(airport.ident));
}

void RouteController::routeSetDestinationInternal(const map::MapAirport& airport)
{
  FlightplanEntry entry;
  entryBuilder->buildFlightplanEntry(airport, entry);

  Flightplan& flightplan = route.getFlightplan();
  if(!flightplan.isEmpty())
  {
    // Remove current destination
    const FlightplanEntry& last = flightplan.getEntries().last();
    if(last.getWaypointType() == pln::entry::AIRPORT &&
       flightplan.getDestinationIdent() == last.getIcaoIdent() && flightplan.getEntries().size() > 1)
    {
      // Remove the last airport if it is set as destination
      flightplan.getEntries().removeLast();
      route.removeLast();
    }
  }

  flightplan.getEntries().append(entry);

  const RouteLeg *lastLeg = nullptr;
  if(flightplan.getEntries().size() > 1)
    // Set predecessor if route has entries
    lastLeg = &route.at(route.size() - 1);

  RouteLeg routeLeg(&flightplan);
  routeLeg.createFromAirport(flightplan.getEntries().size() - 1, airport, lastLeg);
  route.append(routeLeg);

  updateStartPositionBestRunway(false /* force */, false /* undo */);
}

void RouteController::routeAttachProcedure(proc::MapProcedureLegs legs, const QString& sidStarRunway)
{
  qDebug() << Q_FUNC_INFO
           << legs.approachType << legs.approachFixIdent << legs.approachSuffix << legs.approachArincName
           << legs.transitionType << legs.transitionFixIdent;

  RouteCommand *undoCommand = nullptr;

  // if(route.getFlightplan().canSaveProcedures())
  undoCommand = preChange(tr("Add Procedure"));

  // Airport id in legs is from nav database - convert to simulator database
  map::MapAirport airportSim;
  NavApp::getAirportQueryNav()->getAirportById(airportSim, legs.ref.airportId);
  mapQuery->getAirportSimReplace(airportSim);

  if(legs.mapType & proc::PROCEDURE_STAR || legs.mapType & proc::PROCEDURE_ARRIVAL)
  {
    if(route.isEmpty() || route.last().getMapObjectType() != map::AIRPORT || route.last().getId() != airportSim.id)
    {
      // No route, no destination airport or different airport
      route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);
      routeSetDestinationInternal(airportSim);
    }
    // Will take care of the flight plan entries too
    if(legs.mapType & proc::PROCEDURE_STAR)
    {
      // Assign runway for SID/STAR than can have multiple runways
      NavApp::getProcedureQuery()->insertSidStarRunway(legs, sidStarRunway);
      route.setStarProcedureLegs(legs);
    }
    if(legs.mapType & proc::PROCEDURE_ARRIVAL)
      route.setArrivalProcedureLegs(legs);

    route.updateProcedureLegs(entryBuilder, true /* clear old procedure properties */);
  }
  else if(legs.mapType & proc::PROCEDURE_DEPARTURE)
  {
    if(route.isEmpty() || route.first().getMapObjectType() != map::AIRPORT || route.first().getId() != airportSim.id)
    {
      // No route, no departure airport or different airport
      route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);
      routeSetDepartureInternal(airportSim);
    }
    // Assign runway for SID/STAR than can have multiple runways
    NavApp::getProcedureQuery()->insertSidStarRunway(legs, sidStarRunway);

    // Will take care of the flight plan entries too
    route.setDepartureProcedureLegs(legs);
    route.updateProcedureLegs(entryBuilder, true /* clear old procedure properties */);
  }
  updateErrorLabel();
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);
  routeToFlightPlan();

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  route.updateActiveLegAndPos(true /* force update */);
  updateTableModel();

  postChange(undoCommand);
  NavApp::updateWindowTitle();

  qDebug() << Q_FUNC_INFO << route.getFlightplan().getProperties();

  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Added procedure to flight plan."));
}

void RouteController::routeAdd(int id, atools::geo::Pos userPos, map::MapObjectTypes type, int legIndex)
{
  qDebug() << Q_FUNC_INFO << "user pos" << userPos << "id" << id
           << "type" << type << "leg index" << legIndex;

  FlightplanEntry entry;
  entryBuilder->buildFlightplanEntry(id, userPos, type, entry, -1);

  int insertIndex = calculateInsertIndex(entry.getPosition(), legIndex);

  routeAddInternal(entry, insertIndex);
}

void RouteController::routeAddInternal(const FlightplanEntry& entry, int insertIndex)
{
  qDebug() << Q_FUNC_INFO << "insertIndex" << insertIndex;

  RouteCommand *undoCommand = preChange(tr("Add Waypoint"));

  Flightplan& flightplan = route.getFlightplan();
  flightplan.getEntries().insert(insertIndex, entry);
  eraseAirway(insertIndex);
  eraseAirway(insertIndex + 1);

  const RouteLeg *lastLeg = nullptr;

  if(flightplan.isEmpty() && insertIndex > 0)
    lastLeg = &route.at(insertIndex - 1);
  RouteLeg routeLeg(&flightplan);
  routeLeg.createFromDatabaseByEntry(insertIndex, lastLeg);

  route.insert(insertIndex, routeLeg);

  proc::MapProcedureTypes procs = affectedProcedures({insertIndex});
  route.removeProcedureLegs(procs);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);
  // Force update of start if departure airport was added
  updateStartPositionBestRunway(false /* force */, false /* undo */);
  routeToFlightPlan();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  route.updateActiveLegAndPos(true /* force update */);
  updateTableModel();

  postChange(undoCommand);
  NavApp::updateWindowTitle();

  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Added waypoint to flight plan."));
}

int RouteController::calculateInsertIndex(const atools::geo::Pos& pos, int legIndex)
{
  Flightplan& flightplan = route.getFlightplan();

  int insertIndex = -1;
  if(legIndex == map::INVALID_INDEX_VALUE)
    // Append
    insertIndex = route.size();
  else if(legIndex == -1)
  {
    if(flightplan.isEmpty())
      // First is  departure
      insertIndex = 0;
    else if(flightplan.getEntries().size() == 1)
      // Keep first as departure
      insertIndex = 1;
    else
    {
      // No leg index given - search for nearest available route leg
      atools::geo::LineDistance result;
      int nearestlegIndex = route.getNearestRouteLegResult(pos, result, true /* ignoreNotEditable */);

      switch(result.status)
      {
        case atools::geo::INVALID:
          insertIndex = 0;
          break;
        case atools::geo::ALONG_TRACK:
          insertIndex = nearestlegIndex;
          break;
        case atools::geo::BEFORE_START:
          if(nearestlegIndex == 1)
            // Add before departure
            insertIndex = 0;
          else
            insertIndex = nearestlegIndex;
          break;
        case atools::geo::AFTER_END:
          if(nearestlegIndex == route.size() - 1)
            insertIndex = nearestlegIndex + 1;
          else
            insertIndex = nearestlegIndex;
          break;
      }
    }
  }
  else
    // Adjust and use given leg index (insert after index point)
    insertIndex = legIndex + 1;

  qDebug() << "insertIndex" << insertIndex << "pos" << pos;

  return insertIndex;
}

void RouteController::routeReplace(int id, atools::geo::Pos userPos, map::MapObjectTypes type,
                                   int legIndex)
{
  qDebug() << Q_FUNC_INFO << "user pos" << userPos << "id" << id
           << "type" << type << "leg index" << legIndex;

  RouteCommand *undoCommand = preChange(tr("Change Waypoint"));

  const FlightplanEntry oldEntry = route.getFlightplan().getEntries().at(legIndex);

  FlightplanEntry entry;
  entryBuilder->buildFlightplanEntry(id, userPos, type, entry, -1);

  if(oldEntry.getWaypointType() == atools::fs::pln::entry::USER &&
     entry.getWaypointType() == atools::fs::pln::entry::USER)
    entry.setWaypointId(oldEntry.getWaypointId());

  Flightplan& flightplan = route.getFlightplan();

  flightplan.getEntries().replace(legIndex, entry);

  const RouteLeg *lastLeg = nullptr;
  if(legIndex > 0 && !route.isFlightplanEmpty())
    // Get predecessor of replaced entry
    lastLeg = &route.at(legIndex - 1);

  RouteLeg routeLeg(&flightplan);
  routeLeg.createFromDatabaseByEntry(legIndex, lastLeg);

  route.replace(legIndex, routeLeg);
  eraseAirway(legIndex);
  eraseAirway(legIndex + 1);

  if(legIndex == route.size() - 1)
    route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

  if(legIndex == 0)
    route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

  // Force update of start if departure airport was changed
  updateStartPositionBestRunway(legIndex == 0 /* force */, false /* undo */);

  routeToFlightPlan();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  route.updateActiveLegAndPos(true /* force update */);
  updateTableModel();

  postChange(undoCommand);
  NavApp::updateWindowTitle();
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Replaced waypoint in flight plan."));
}

void RouteController::routeDelete(int index)
{
  qDebug() << Q_FUNC_INFO << index;

  RouteCommand *undoCommand = preChange(tr("Delete"));

  route.getFlightplan().getEntries().removeAt(index);

  route.removeAt(index);
  eraseAirway(index);

  if(index == route.size())
    route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

  if(index == 0)
    route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */, false /* adjustRouteType */);

  // Force update of start if departure airport was removed
  updateStartPositionBestRunway(index == 0 /* force */, false /* undo */);
  routeToFlightPlan();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  updateTableModel();

  postChange(undoCommand);
  NavApp::updateWindowTitle();
  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Removed waypoint from flight plan."));
}

/* Update airway attribute in flight plan entry and return minimum altitude for this airway segment */
void RouteController::updateFlightplanEntryAirway(int airwayId, FlightplanEntry& entry)
{
  map::MapAirway airway;
  mapQuery->getAirwayById(airway, airwayId);
  entry.setAirway(airway.name);
}

/* Copy all data from route map objects and widgets to the flight plan */
void RouteController::routeToFlightPlan()
{
  Flightplan& flightplan = route.getFlightplan();

  if(route.isEmpty())
    flightplan.clear();
  else
  {
    QString departureIcao, destinationIcao;

    const RouteLeg& firstLeg = route.first();
    if(firstLeg.getMapObjectType() == map::AIRPORT)
    {
      departureIcao = firstLeg.getAirport().ident;
      flightplan.setDepartureAiportName(firstLeg.getAirport().name);
      flightplan.setDepartureIdent(departureIcao);

      if(route.hasDepartureParking())
      {
        flightplan.setDepartureParkingName(map::parkingNameForFlightplan(firstLeg.getDepartureParking()));
        flightplan.setDeparturePosition(firstLeg.getDepartureParking().position, firstLeg.getPosition().getAltitude());
      }
      else if(route.hasDepartureStart())
      {
        // Use runway name or helipad number
        flightplan.setDepartureParkingName(firstLeg.getDepartureStart().runwayName);
        flightplan.setDeparturePosition(firstLeg.getDepartureStart().position, firstLeg.getPosition().getAltitude());
      }
      else
        // No start position and no parking - use airport/navaid position
        flightplan.setDeparturePosition(firstLeg.getPosition());
    }
    else
    {
      // Invalid departure
      flightplan.setDepartureAiportName(QString());
      flightplan.setDepartureIdent(QString());
      flightplan.setDepartureParkingName(QString());
      flightplan.setDeparturePosition(Pos(), 0.f);
    }

    const RouteLeg& lastLeg = route.last();
    if(lastLeg.getMapObjectType() == map::AIRPORT)
    {
      destinationIcao = lastLeg.getAirport().ident;
      flightplan.setDestinationAiportName(lastLeg.getAirport().name);
      flightplan.setDestinationIdent(destinationIcao);
      flightplan.setDestinationPosition(lastLeg.getPosition());
    }
    else
    {
      // Invalid destination
      flightplan.setDestinationAiportName(QString());
      flightplan.setDestinationIdent(QString());
      flightplan.setDestinationPosition(Pos());
    }

    // <Descr>LFHO, EDRJ</Descr>
    flightplan.setDescription(departureIcao + ", " + destinationIcao);

    // <Title>LFHO to EDRJ</Title>
    flightplan.setTitle(departureIcao + " to " + destinationIcao);
  }
}

/* Copy type and cruise altitude from widgets to flight plan */
void RouteController::updateFlightplanFromWidgets()
{
  assignAircraftPerformance(route.getFlightplan());
  updateFlightplanFromWidgets(route.getFlightplan());
}

void RouteController::assignAircraftPerformance(Flightplan& flightplan)
{
  const atools::fs::perf::AircraftPerf perf = NavApp::getAircraftPerfController()->getAircraftPerformance();

  flightplan.getProperties().insert(atools::fs::pln::AIRCRAFT_PERF_NAME, perf.getName());
  flightplan.getProperties().insert(atools::fs::pln::AIRCRAFT_PERF_TYPE, perf.getAircraftType());
  flightplan.getProperties().insert(atools::fs::pln::AIRCRAFT_PERF_FILE,
                                    NavApp::getAircraftPerfController()->getCurrentFilepath());
}

void RouteController::updateFlightplanFromWidgets(Flightplan& flightplan)
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  flightplan.setFlightplanType(ui->comboBoxRouteType->currentIndex() ==
                               0 ? atools::fs::pln::IFR : atools::fs::pln::VFR);
  flightplan.setCruisingAltitude(ui->spinBoxRouteAlt->value());
}

QIcon RouteController::iconForLeg(const RouteLeg& leg, float size) const
{
  int sizeInt = atools::roundToInt(size);
  QIcon icon;
  if(leg.getMapObjectType() == map::AIRPORT)
    icon = symbolPainter->createAirportIcon(leg.getAirport(), sizeInt);
  else if(leg.getVor().isValid())
    icon = symbolPainter->createVorIcon(leg.getVor(), sizeInt);
  else if(leg.getNdb().isValid())
    icon = ndbIcon;
  else if(leg.getWaypoint().isValid())
    icon = waypointIcon;
  else if(leg.getMapObjectType() == map::USERPOINTROUTE)
    icon = userpointIcon;
  else if(leg.getMapObjectType() == map::INVALID)
    icon = invalidIcon;
  else if(leg.isAnyProcedure())
    icon = procedureIcon;

  return icon;
}

/* Update table view model completely */
void RouteController::updateTableModel()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  model->removeRows(0, model->rowCount());
  float totalDistance = route.getTotalDistance();

  int row = 0;
  float cumulatedDistance = 0.f;

  QList<QStandardItem *> itemRow;
  for(int i = rc::FIRST_COLUMN; i <= rc::LAST_COLUMN; i++)
    itemRow.append(nullptr);

  for(int i = 0; i < route.size(); i++)
  {
    const RouteLeg& leg = route.at(i);
    bool afterArrivalAirport = route.isAirportAfterArrival(i);

    // Ident ===========================================
    QString identStr;
    if(leg.isAnyProcedure())
      // Get ident with IAF, FAF or other indication
      identStr = proc::procedureLegFixStr(leg.getProcedureLeg());
    else
      identStr = leg.getIdent();

    QStandardItem *ident = new QStandardItem(iconForLeg(leg, iconSize), identStr);
    QFont f = ident->font();
    f.setBold(true);
    ident->setFont(f);
    ident->setTextAlignment(Qt::AlignRight);

    if(leg.getMapObjectType() == map::INVALID)
      ident->setForeground(Qt::red);

    itemRow[rc::IDENT] = ident;

    // Region, navaid name, procedure type ===========================================
    itemRow[rc::REGION] = new QStandardItem(leg.getRegion());
    itemRow[rc::NAME] = new QStandardItem(leg.getName());
    itemRow[rc::PROCEDURE] = new QStandardItem(route.getProcedureLegText(leg.getProcedureType()));

    // Airway or leg type and restriction ===========================================
    if(leg.isRoute())
    {
      itemRow[rc::AIRWAY_OR_LEGTYPE] = new QStandardItem(leg.getAirwayName());
      if(leg.getAirway().isValid() && leg.getAirway().minAltitude > 0)
        itemRow[rc::RESTRICTION] = new QStandardItem(Unit::altFeet(leg.getAirway().minAltitude, false));
    }
    else
    {
      itemRow[rc::AIRWAY_OR_LEGTYPE] = new QStandardItem(proc::procedureLegTypeStr(leg.getProcedureLegType()));

      QString restrictions;
      if(leg.getProcedureLegAltRestr().isValid())
        restrictions.append(proc::altRestrictionTextShort(leg.getProcedureLegAltRestr()));
      if(leg.getProcedureLeg().speedRestriction.isValid())
        restrictions.append("/" + proc::speedRestrictionTextShort(leg.getProcedureLeg().speedRestriction));

      itemRow[rc::RESTRICTION] = new QStandardItem(restrictions);
    }

    // Get ILS for approach runway if it marks the end of an ILS or localizer approach procedure
    QVector<map::MapIls> ilsByAirportAndRunway;
    if(route.getArrivalLegs().hasIlsGuidance() &&
       leg.isAnyProcedure() && leg.getProcedureLeg().isApproach() && leg.getRunwayEnd().isValid())
      route.getApproachRunwayEndAndIls(ilsByAirportAndRunway);

    // VOR/NDB type ===========================
    if(leg.getVor().isValid())
      itemRow[rc::TYPE] = new QStandardItem(map::vorFullShortText(leg.getVor()));
    else if(leg.getNdb().isValid())
      itemRow[rc::TYPE] = new QStandardItem(map::ndbFullShortText(leg.getNdb()));
    else if(leg.isAnyProcedure() && !(leg.getProcedureType() & proc::PROCEDURE_MISSED) &&
            leg.getRunwayEnd().isValid())
    {
      // Build string for ILS type
      QSet<QString> texts;
      for(const map::MapIls& ils : ilsByAirportAndRunway)
      {
        QStringList txt(ils.slope > 0.f ? tr("ILS") : tr("LOC"));
        if(ils.hasDme)
          txt.append("DME");
        texts.insert(txt.join("/"));
      }

      itemRow[rc::TYPE] = new QStandardItem(texts.toList().join(","));
    }

    // VOR/NDB frequency =====================
    if(leg.getVor().isValid())
    {
      if(leg.getVor().tacan)
        itemRow[rc::FREQ] = new QStandardItem(leg.getVor().channel);
      else
        itemRow[rc::FREQ] = new QStandardItem(QLocale().toString(leg.getFrequency() / 1000.f, 'f', 2));
    }
    else if(leg.getNdb().isValid())
      itemRow[rc::FREQ] = new QStandardItem(QLocale().toString(leg.getFrequency() / 100.f, 'f', 1));
    else if(leg.isAnyProcedure() && !(leg.getProcedureType() & proc::PROCEDURE_MISSED) &&
            leg.getRunwayEnd().isValid())
    {
      // Add ILS frequencies
      QSet<QString> texts;
      for(const map::MapIls& ils : ilsByAirportAndRunway)
        texts.insert(QLocale().toString(ils.frequency / 1000.f, 'f', 2));

      itemRow[rc::FREQ] = new QStandardItem(texts.toList().join(","));
    }

    // VOR/NDB range =====================
    if(leg.getRange() > 0 && (leg.getVor().isValid() || leg.getNdb().isValid()))
      itemRow[rc::RANGE] = new QStandardItem(Unit::distNm(leg.getRange(), false));

    // Course =====================
    if(row > 0 && !afterArrivalAirport && leg.getDistanceTo() < map::INVALID_DISTANCE_VALUE &&
       leg.getDistanceTo() > 0.f)
    {
      if(leg.getCourseToMag() < map::INVALID_COURSE_VALUE)
        itemRow[rc::COURSE] = new QStandardItem(QLocale().toString(leg.getCourseToMag(), 'f', 0));
      if(leg.getCourseToRhumbMag() < map::INVALID_COURSE_VALUE)
        itemRow[rc::DIRECT] = new QStandardItem(QLocale().toString(leg.getCourseToRhumbMag(), 'f', 0));
    }

    if(!afterArrivalAirport)
    {
      if(leg.getDistanceTo() < map::INVALID_DISTANCE_VALUE) // Distance =====================
      {
        cumulatedDistance += leg.getDistanceTo();
        itemRow[rc::DIST] = new QStandardItem(Unit::distNm(leg.getDistanceTo(), false));

        if(!leg.getProcedureLeg().isMissed())
        {
          float remaining = totalDistance - cumulatedDistance;
          if(remaining < 0.f)
            remaining = 0.f; // Catch the -0 case due to rounding errors
          itemRow[rc::REMAINING_DISTANCE] = new QStandardItem(Unit::distNm(remaining, false));
        }
      }
    }

    if(leg.isAnyProcedure())
      itemRow[rc::REMARKS] = new QStandardItem(proc::procedureLegRemark(leg.getProcedureLeg()));

    // Travel time and ETA are updated in updateModelRouteTime

    // Create empty items for missing fields
    for(int col = rc::FIRST_COLUMN; col <= rc::LAST_COLUMN; col++)
    {
      if(itemRow[col] == nullptr)
        itemRow[col] = new QStandardItem();
      itemRow[col]->setFlags(itemRow[col]->flags() &
                             ~(Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
    }

    // Align cells to the right - rest is aligned in updateModelRouteTimeFuel
    itemRow[rc::REGION]->setTextAlignment(Qt::AlignRight);
    itemRow[rc::REMAINING_DISTANCE]->setTextAlignment(Qt::AlignRight);
    itemRow[rc::DIST]->setTextAlignment(Qt::AlignRight);
    itemRow[rc::COURSE]->setTextAlignment(Qt::AlignRight);
    itemRow[rc::DIRECT]->setTextAlignment(Qt::AlignRight);
    itemRow[rc::RANGE]->setTextAlignment(Qt::AlignRight);
    itemRow[rc::FREQ]->setTextAlignment(Qt::AlignRight);
    itemRow[rc::RESTRICTION]->setTextAlignment(Qt::AlignRight);

    model->appendRow(itemRow);

    for(int col = rc::FIRST_COLUMN; col <= rc::LAST_COLUMN; col++)
      itemRow[col] = nullptr;
    row++;
  }

  updateModelRouteTimeFuel();

  Flightplan& flightplan = route.getFlightplan();

  if(!flightplan.isEmpty())
  {
    // Set spin box and block signals to avoid recursive call
    {
      QSignalBlocker blocker(ui->spinBoxRouteAlt);
      Q_UNUSED(blocker);
      ui->spinBoxRouteAlt->setValue(flightplan.getCruisingAltitude());
    }

    { // Set combo box and block signals to avoid recursive call
      QSignalBlocker blocker(ui->comboBoxRouteType);
      Q_UNUSED(blocker);
      if(flightplan.getFlightplanType() == atools::fs::pln::IFR)
        ui->comboBoxRouteType->setCurrentIndex(0);
      else if(flightplan.getFlightplanType() == atools::fs::pln::VFR)
        ui->comboBoxRouteType->setCurrentIndex(1);
    }
  }

  highlightProcedureItems();
  highlightNextWaypoint(route.getActiveLegIndexCorrected());
  updateWindowLabel();
}

/* Update travel times in table view model after speed change */
void RouteController::updateModelRouteTimeFuel()
{
  using atools::fs::perf::AircraftPerf;
  const RouteAltitude& altitudeLegs = route.getAltitudeLegs();
  if(altitudeLegs.isEmpty())
    return;

  int row = 0;
  float cumulatedDistance = 0.f;
  float cumulatedTravelTime = 0.f;

  bool setValues = !NavApp::isCollectingPerformance() && !altitudeLegs.hasErrors();
  const AircraftPerf& perf = NavApp::getAircraftPerformance();
  float totalFuelLbsOrGal = altitudeLegs.getTripFuel();

  if(setValues)
  {
    totalFuelLbsOrGal *= perf.getContingencyFuelFactor();
    totalFuelLbsOrGal += perf.getExtraFuel() + perf.getReserveFuel();
  }

  int widthLegTime = view->columnWidth(rc::LEG_TIME);
  int widthEta = view->columnWidth(rc::ETA);
  int widthFuelWeight = view->columnWidth(rc::FUEL_WEIGHT);
  int widthFuelVol = view->columnWidth(rc::FUEL_VOLUME);

  for(int i = 0; i < route.size(); i++)
  {
    if(!setValues)
    {
      model->setItem(row, rc::LEG_TIME, new QStandardItem());
      model->setItem(row, rc::ETA, new QStandardItem());
      model->setItem(row, rc::FUEL_WEIGHT, new QStandardItem());
      model->setItem(row, rc::FUEL_VOLUME, new QStandardItem());
    }
    else if(!route.isAirportAfterArrival(row))
    {
      const RouteLeg& leg = route.at(i);
      float travelTime = altitudeLegs.at(i).getTravelTimeHours();
      if(row == 0 || !(travelTime < map::INVALID_TIME_VALUE) || leg.getProcedureLeg().isMissed())
        model->setItem(row, rc::LEG_TIME, new QStandardItem());
      else
      {
        QString txt = formatter::formatMinutesHours(travelTime);
#ifdef DEBUG_INFORMATION_LEGTIME
        txt += " [" + QString::number(travelTime * 3600., 'f', 0) + "]";
#endif
        QStandardItem *item = new QStandardItem(txt);
        item->setTextAlignment(Qt::AlignRight);
        model->setItem(row, rc::LEG_TIME, item);
      }

      if(!leg.getProcedureLeg().isMissed())
      {
        cumulatedDistance += leg.getDistanceTo();
        cumulatedTravelTime += travelTime;
        QString txt = formatter::formatMinutesHours(cumulatedTravelTime);
#ifdef DEBUG_INFORMATION_LEGTIME
        txt += " [" + QString::number(cumulatedTravelTime * 3600., 'f', 0) + "]";
#endif
        QStandardItem *item = new QStandardItem(txt);
        item->setTextAlignment(Qt::AlignRight);
        model->setItem(row, rc::ETA, item);

        totalFuelLbsOrGal -= altitudeLegs.at(i).getFuel();
        float weight = 0.f, vol = 0.f;
        if(perf.useFuelAsVolume())
        {
          weight = AircraftPerf::fromGalToLbs(perf.isJetFuel(), totalFuelLbsOrGal);
          vol = totalFuelLbsOrGal;
        }
        else
        {
          weight = totalFuelLbsOrGal;
          vol = AircraftPerf::fromLbsToGal(perf.isJetFuel(), totalFuelLbsOrGal);
        }

        if(atools::almostEqual(vol, 0.f, 0.01f))
          // Avoid -0 case
          vol = 0.f;
        if(atools::almostEqual(weight, 0.f, 0.01f))
          // Avoid -0 case
          weight = 0.f;

        txt = perf.isFuelFlowValid() ? Unit::weightLbs(weight, false /* no unit */) : QString();
        item = new QStandardItem(txt);
        item->setTextAlignment(Qt::AlignRight);
        model->setItem(row, rc::FUEL_WEIGHT, item);

        txt = perf.isFuelFlowValid() ? Unit::volGallon(vol, false /* no unit */) : QString();
        item = new QStandardItem(txt);
        item->setTextAlignment(Qt::AlignRight);
        model->setItem(row, rc::FUEL_VOLUME, item);
      }
    }

    row++;
  }

  view->setColumnWidth(rc::LEG_TIME, widthLegTime);
  view->setColumnWidth(rc::ETA, widthEta);
  view->setColumnWidth(rc::FUEL_WEIGHT, widthFuelWeight);
  view->setColumnWidth(rc::FUEL_VOLUME, widthFuelVol);
}

void RouteController::disconnectedFromSimulator()
{
  qDebug() << Q_FUNC_INFO;

  route.resetActive();
  highlightNextWaypoint(-1);
  emit routeChanged(false);
}

void RouteController::simDataChanged(const atools::fs::sc::SimConnectData& simulatorData)
{
  if(atools::almostNotEqual(QDateTime::currentDateTime().toMSecsSinceEpoch(),
                            lastSimUpdate, static_cast<qint64>(MIN_SIM_UPDATE_TIME_MS)))
  {
    if(simulatorData.isUserAircraftValid())
    {
      const atools::fs::sc::SimConnectUserAircraft& aircraft = simulatorData.getUserAircraftConst();

      // Sequence only for airborne airplanes
      // Use more than one parameter since first X-Plane data packets are unreliable
      if(aircraft.isFlying())
      {
        map::PosCourse position(aircraft.getPosition(), aircraft.getTrackDegTrue());
        int previousRouteLeg = route.getActiveLegIndexCorrected();
        route.updateActiveLegAndPos(position);
        int routeLeg = route.getActiveLegIndexCorrected();

        if(routeLeg != previousRouteLeg)
        {
          // Use corrected indexes to highlight initial fix
          qDebug() << "new route leg" << previousRouteLeg << routeLeg;
          highlightNextWaypoint(routeLeg);

          if(OptionData::instance().getFlags2() & opts::ROUTE_CENTER_ACTIVE_LEG)
            view->scrollTo(model->index(std::max(routeLeg - 1, 0), 0), QAbstractItemView::PositionAtTop);
        }
      }
    }
    lastSimUpdate = QDateTime::currentDateTime().toMSecsSinceEpoch();
  }
}

/* */
void RouteController::highlightNextWaypoint(int nearestLegIndex)
{
  for(int row = 0; row < model->rowCount(); ++row)
  {
    for(int col = rc::FIRST_COLUMN; col <= rc::LAST_COLUMN; ++col)
    {
      QStandardItem *item = model->item(row, col);
      if(item != nullptr)
      {
        item->setBackground(Qt::NoBrush);
        // Keep first column bold
        if(item->font().bold() && col != 0)
        {
          QFont font = item->font();
          font.setBold(false);
          item->setFont(font);
        }
      }
    }
  }

  if(!route.isEmpty())
  {
    if(nearestLegIndex >= 0 && nearestLegIndex < route.size())
    {
      QColor color = NavApp::isCurrentGuiStyleNight() ?
                     mapcolors::nextWaypointColorDark : mapcolors::nextWaypointColor;

      for(int col = rc::FIRST_COLUMN; col <= rc::LAST_COLUMN; ++col)
      {
        QStandardItem *item = model->item(nearestLegIndex, col);
        if(item != nullptr)
        {
          item->setBackground(color);
          if(!item->font().bold())
          {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
          }
        }
      }
    }
  }
  highlightProcedureItems();
}

/* Set colors for procedures and missing objects like waypoints and airways */
void RouteController::highlightProcedureItems()
{
  for(int row = 0; row < model->rowCount(); ++row)
  {
    for(int col = 0; col < model->columnCount(); ++col)
    {
      QStandardItem *item = model->item(row, col);
      if(item != nullptr)
      {
        const RouteLeg& leg = route.at(row);
        if(leg.isAnyProcedure())
        {
          if(leg.getProcedureLeg().isMissed())
            item->setForeground(NavApp::isCurrentGuiStyleNight() ?
                                mapcolors::routeProcedureMissedTableColorDark :
                                mapcolors::routeProcedureMissedTableColor);
          else
            item->setForeground(NavApp::isCurrentGuiStyleNight() ?
                                mapcolors::routeProcedureTableColorDark :
                                mapcolors::routeProcedureTableColor);
        }
        else if((col == rc::IDENT && leg.getMapObjectType() == map::INVALID) ||
                (col == rc::AIRWAY_OR_LEGTYPE && leg.isRoute() && leg.isAirwaySetAndInvalid()))
        {
          item->setForeground(Qt::red);
          QFont font = item->font();
          font.setBold(true);
          item->setFont(font);
        }
        else
          item->setForeground(QApplication::palette().color(QPalette::Normal, QPalette::Text));
      }
    }
  }
}

/* Update the dock window top level label */
void RouteController::updateWindowLabel()
{
  QString text = buildFlightplanLabel() + "<br/>" + buildFlightplanLabel2();
  NavApp::getMainUi()->labelRouteInfo->setText(text);
}

QString RouteController::buildFlightplanLabel(bool print, bool titleOnly) const
{
  const Flightplan& flightplan = route.getFlightplan();

  QString departure(tr("Invalid")), destination(tr("Invalid")), approach;

  if(!flightplan.isEmpty())
  {
    QString starRunway, approachRunway;

    // Add departure to text ==============================================================
    if(route.hasValidDeparture())
    {
      departure = flightplan.getDepartureAiportName() + " (" + flightplan.getDepartureIdent() + ")";

      if(route.first().getDepartureParking().isValid())
        departure += " " + map::parkingNameNumberType(route.first().getDepartureParking());
      else if(route.first().getDepartureStart().isValid())
      {
        const map::MapStart& start = route.first().getDepartureStart();
        if(route.hasDepartureHelipad())
          departure += tr(" Helipad %1").arg(start.runwayName);
        else if(!start.runwayName.isEmpty())
          departure += tr(" Runway %1").arg(start.runwayName);
        else
          departure += tr(" Unknown Start");
      }
    }
    else
      departure = QString("%1 (%2)").
                  arg(flightplan.getEntries().first().getIcaoIdent()).
                  arg(flightplan.getEntries().first().getWaypointTypeAsString());

    // Add destination to text ==============================================================
    if(route.hasValidDestination())
    {
      destination = flightplan.getDestinationAiportName() + " (" + flightplan.getDestinationIdent() + ")";
      // airportId = route.last().getAirport().id;
    }
    else
      destination = QString("%1 (%2)").
                    arg(flightplan.getEntries().last().getIcaoIdent()).
                    arg(flightplan.getEntries().last().getWaypointTypeAsString());

    if(!titleOnly)
    {
      // Add procedures to text ==============================================================
      const proc::MapProcedureLegs& arrivalLegs = route.getArrivalLegs();
      const proc::MapProcedureLegs& starLegs = route.getStarLegs();
      if(route.hasAnyProcedure())
      {
        QStringList procedureText;
        QVector<bool> boldTextFlag;

        const proc::MapProcedureLegs& departureLegs = route.getDepartureLegs();
        if(!departureLegs.isEmpty())
        {
          // Add departure procedure to text
          if(!departureLegs.runwayEnd.isValid())
          {
            boldTextFlag << false;
            procedureText.append(tr("Depart via SID"));
          }
          else
          {
            boldTextFlag << false << true << false;
            procedureText.append(tr("Depart runway"));
            procedureText.append(departureLegs.runwayEnd.name);
            procedureText.append(tr("via SID"));
          }

          QString sid(departureLegs.approachFixIdent);
          if(!departureLegs.transitionFixIdent.isEmpty())
            sid += "." + departureLegs.transitionFixIdent;
          boldTextFlag << true;
          procedureText.append(sid);

          if(arrivalLegs.mapType & proc::PROCEDURE_ARRIVAL_ALL || starLegs.mapType & proc::PROCEDURE_ARRIVAL_ALL)
          {
            boldTextFlag << false;
            procedureText.append(tr("."));
          }
        }

        // Add arrival procedures procedure to text
        // STAR
        if(!starLegs.isEmpty())
        {
          if(print)
          {
            // Add line break between departure and arrival for printing
            boldTextFlag << false;
            procedureText.append("<br/>");
          }

          boldTextFlag << false << true;
          procedureText.append(tr("Arrive via STAR"));

          QString star(starLegs.approachFixIdent);
          if(!starLegs.transitionFixIdent.isEmpty())
            star += "." + starLegs.transitionFixIdent;
          procedureText.append(star);

          starRunway = starLegs.procedureRunway;

          if(!(arrivalLegs.mapType & proc::PROCEDURE_APPROACH))
          {
            boldTextFlag << false << true;
            procedureText.append(tr("at runway"));
            procedureText.append(starLegs.procedureRunway);
          }
          else if(!starLegs.procedureRunway.isEmpty())
          {
            boldTextFlag << false;
            procedureText.append(tr("(<b>%1</b>)").arg(starLegs.procedureRunway));
          }

          if(!(arrivalLegs.mapType & proc::PROCEDURE_APPROACH))
          {
            boldTextFlag << false;
            procedureText.append(tr("."));
          }
        }

        if(arrivalLegs.mapType & proc::PROCEDURE_TRANSITION)
        {
          boldTextFlag << false << true;
          procedureText.append(!starLegs.isEmpty() ? tr("via") : tr("Via"));
          procedureText.append(arrivalLegs.transitionFixIdent);
        }

        if(arrivalLegs.mapType & proc::PROCEDURE_APPROACH)
        {
          boldTextFlag << false;
          procedureText.append((arrivalLegs.mapType & proc::PROCEDURE_TRANSITION ||
                                !starLegs.isEmpty()) ? tr("and") : tr("Via"));

          // Type and suffix =======================
          QString type(arrivalLegs.approachType);
          if(!arrivalLegs.approachSuffix.isEmpty())
            type += tr("-%1").arg(arrivalLegs.approachSuffix);

          boldTextFlag << true;
          procedureText.append(type);

          boldTextFlag << true;
          procedureText.append(arrivalLegs.approachFixIdent);

          if(!arrivalLegs.approachArincName.isEmpty())
          {
            boldTextFlag << true;
            procedureText.append(tr("(%1)").arg(arrivalLegs.approachArincName));
          }

          // Runway =======================
          if(arrivalLegs.runwayEnd.isValid() && !arrivalLegs.runwayEnd.name.isEmpty())
          {
            // Add runway for approach
            boldTextFlag << false << true << false;
            procedureText.append(procedureText.isEmpty() ? tr("To runway") : tr("to runway"));
            procedureText.append(arrivalLegs.runwayEnd.name);
            procedureText.append(tr("."));
          }
          else
          {
            // Add runway text
            boldTextFlag << false;
            procedureText.append(procedureText.isEmpty() ? tr("To runway.") : tr("to runway."));
          }
          approachRunway = arrivalLegs.runwayEnd.name;
        }

        if(!approachRunway.isEmpty() && !starRunway.isEmpty() && approachRunway != starRunway)
        {
          boldTextFlag << true;
          procedureText.append(atools::util::HtmlBuilder::errorMessage(tr("Runway mismatch: STAR %1 ≠ Approach %2.").
                                                                       arg(starRunway).arg(approachRunway)));
        }

        for(int i = 0; i < procedureText.size(); i++)
        {
          if(boldTextFlag.at(i))
            procedureText[i] = "<b>" + procedureText.at(i) + "</b>";
        }
        approach = procedureText.join(" ");
      }
    }
  }

  QString title(tr("No Flight Plan loaded."));
  if(!flightplan.isEmpty())
  {
    if(print)
      title = tr("<h2>%1 to %2</h2>").arg(departure).arg(destination);
    else
      title = tr("<b>%1</b> to <b>%2</b>").arg(departure).arg(destination);
  }

  if(print)
    return title + (approach.isEmpty() ? QString() : "<p><big>" + approach + "</big></p>");
  else
    return title + (approach.isEmpty() ? QString() : "<br/>" + approach);
}

QString RouteController::buildFlightplanLabel2() const
{
  const Flightplan& flightplan = route.getFlightplan();
  if(!flightplan.isEmpty())
  {
    QString routeType;
    switch(flightplan.getRouteType())
    {
      case atools::fs::pln::LOW_ALTITUDE:
        routeType = tr("Low Altitude");
        break;

      case atools::fs::pln::HIGH_ALTITUDE:
        routeType = tr("High Altitude");
        break;

      case atools::fs::pln::VOR:
        routeType = tr("Radionav");
        break;

      case atools::fs::pln::DIRECT:
        routeType = tr("Direct");
        break;

      case atools::fs::pln::UNKNOWN:
        routeType = tr("Unknown");
        break;
    }

    if(NavApp::getAircraftPerfController()->isDescentValid() && !NavApp::isCollectingPerformance() &&
       route.getAltitudeLegs().getTravelTimeHours() > 0.f)
      return tr("<b>%1, %2</b>, %3").
             arg(Unit::distNm(route.getTotalDistance())).
             arg(formatter::formatMinutesHoursLong(route.getAltitudeLegs().getTravelTimeHours())).
             arg(routeType);
    else
      return tr("<b>%1</b>, %2").
             arg(Unit::distNm(route.getTotalDistance())).
             arg(routeType);
  }
  else
    return QString();
}

/* Reset route and clear undo stack (new route) */
void RouteController::clearRoute()
{
  route.removeProcedureLegs();
  route.getFlightplan().clear();
  route.getFlightplan().getProperties().clear();
  route.resetActive();
  route.clear();
  route.setTotalDistance(0.f);

  routeFilename.clear();
  routeFileFormat = atools::fs::pln::PLN_FSX;

  fileDeparture.clear();
  fileDestination.clear();
  fileIfrVfr = pln::VFR;
  undoStack->clear();
  undoIndex = 0;
  undoIndexClean = 0;
  entryBuilder->setCurUserpointNumber(1);
  updateFlightplanFromWidgets();
}

/* Call this before doing any change to the flight plan that should be undoable */
RouteCommand *RouteController::preChange(const QString& text, rctype::RouteCmdType rcType)
{
  // Clean the flight plan from any procedure entries
  Flightplan flightplan = route.getFlightplan();
  flightplan.removeNoSaveEntries();
  return new RouteCommand(this, flightplan, text, rcType);
}

/* Call this after doing a change to the flight plan that should be undoable */
void RouteController::postChange(RouteCommand *undoCommand)
{
  if(undoCommand == nullptr)
    return;

  // Clean the flight plan from any procedure entries
  Flightplan flightplan = route.getFlightplan();
  flightplan.removeNoSaveEntries();
  undoCommand->setFlightplanAfter(flightplan);

  if(undoIndex < undoIndexClean)
    undoIndexClean = -1;

  // Index and clean index workaround
  undoIndex++;
  qDebug() << "postChange undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
  undoStack->push(undoCommand);
}

/*
 * Select the best runway start position for the departure airport.
 * @param force Update even if a start position is already set
 * @param undo Embed in undo operation
 * @return true if parking was changed
 */
bool RouteController::updateStartPositionBestRunway(bool force, bool undo)
{
  if(route.hasValidDeparture())
  {
    RouteLeg& routeLeg = route.first();

    if(force || (!route.hasDepartureParking() && !route.hasDepartureStart()))
    {
      QString dep, arr;
      route.getRunwayNames(dep, arr);

      // Reset departure position to best runway
      map::MapStart start;
      airportQuery->getBestStartPositionForAirport(start, routeLeg.getAirport().id, dep);

      // Check if the airport has a start position - sone add-on airports don't
      if(start.isValid())
      {
        RouteCommand *undoCommand = nullptr;

        if(undo)
          undoCommand = preChange(tr("Set Start Position"));

        routeLeg.setDepartureStart(start);
        routeToFlightPlan();

        if(undo)
          postChange(undoCommand);
        return true;
      }
    }
  }
  return false;
}

proc::MapProcedureTypes RouteController::affectedProcedures(const QList<int>& indexes)
{
  proc::MapProcedureTypes types = proc::PROCEDURE_NONE;

  for(int index : indexes)
  {
    if(index == 0)
      // Delete SID if departure airport is affected
      types |= proc::PROCEDURE_DEPARTURE;

    if(index >= route.size() - 1)
      // Delete all arrival procedures if destination airport is affected or an new leg is appended after
      types |= proc::PROCEDURE_ARRIVAL_ALL;

    if(index >= 0 && index < route.size())
    {
      const proc::MapProcedureLeg& leg = route.at(index).getProcedureLeg();

      if(leg.isSidTransition())
        types |= proc::PROCEDURE_SID_TRANSITION;

      if(leg.isSid())
        // Delete SID and transition
        types |= proc::PROCEDURE_DEPARTURE;

      if(leg.isStarTransition())
        types |= proc::PROCEDURE_STAR_TRANSITION;

      if(leg.isStar())
        // Delete STAR and transition
        types |= proc::PROCEDURE_STAR_ALL;

      if(leg.isTransition())
        // Delete transition only
        types |= proc::PROCEDURE_TRANSITION;

      if(leg.isApproach() || leg.isMissed())
        // Delete transition and approach
        types |= proc::PROCEDURE_ARRIVAL;
    }
  }

  if(types & proc::PROCEDURE_SID_TRANSITION && route.getDepartureLegs().approachLegs.isEmpty() &&
     !route.getDepartureLegs().approachFixIdent.isEmpty())
    // Remove the empty SID structure too
    types |= proc::PROCEDURE_SID;

  if(types & proc::PROCEDURE_STAR_TRANSITION && route.getStarLegs().approachLegs.isEmpty())
    // Remove the empty STAR structure too
    types |= proc::PROCEDURE_STAR_ALL;

  return types;
}

void RouteController::updateIcons()
{
  ndbIcon = symbolPainter->createNdbIcon(iconSize);
  waypointIcon = symbolPainter->createWaypointIcon(iconSize);
  userpointIcon = symbolPainter->createUserpointIcon(iconSize);
  invalidIcon = symbolPainter->createWaypointIcon(iconSize, mapcolors::routeInvalidPointColor);
  procedureIcon = symbolPainter->createProcedurePointIcon(iconSize);
}

void RouteController::updateErrorLabel()
{
  NavApp::updateErrorLabels();
}

QStringList RouteController::getRouteColumns() const
{
  QStringList colums;
  QHeaderView *header = view->horizontalHeader();

  // Get column names from header and remove line feeds
  for(int col = 0; col < model->columnCount(); col++)
    colums.append(model->headerData(header->logicalIndex(col), Qt::Horizontal).toString().
                  replace("-\n", "-").replace("\n", " "));

  return colums;
}
