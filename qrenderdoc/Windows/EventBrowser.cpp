/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2017 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "EventBrowser.h"
#include <QKeyEvent>
#include <QMenu>
#include <QShortcut>
#include <QTimer>
#include "3rdparty/flowlayout/FlowLayout.h"
#include "Code/CaptureContext.h"
#include "Code/QRDUtils.h"
#include "Code/Resources.h"
#include "ui_EventBrowser.h"

struct EventItemTag
{
  EventItemTag() = default;
  EventItemTag(uint32_t eventID) : EID(eventID), lastEID(eventID) {}
  EventItemTag(uint32_t eventID, uint32_t lastEventID) : EID(eventID), lastEID(lastEventID) {}
  uint32_t EID = 0;
  uint32_t lastEID = 0;
  double duration = -1.0;
  bool current = false;
  bool find = false;
  bool bookmark = false;
};

Q_DECLARE_METATYPE(EventItemTag);

enum
{
  COL_NAME = 0,
  COL_EID = 1,
  COL_DURATION = 2,
};

EventBrowser::EventBrowser(ICaptureContext &ctx, QWidget *parent)
    : QFrame(parent), ui(new Ui::EventBrowser), m_Ctx(ctx)
{
  ui->setupUi(this);

  m_Ctx.AddLogViewer(this);

  clearBookmarks();

  ui->jumpToEID->setFont(Formatter::PreferredFont());
  ui->find->setFont(Formatter::PreferredFont());
  ui->events->setFont(Formatter::PreferredFont());

  ui->events->setColumns(
      {tr("Name"), lit("EID"), lit("Duration - replaced in UpdateDurationColumn")});

  ui->events->header()->resizeSection(COL_EID, 80);

  ui->events->header()->setSectionResizeMode(COL_NAME, QHeaderView::Stretch);
  ui->events->header()->setSectionResizeMode(COL_EID, QHeaderView::Interactive);
  ui->events->header()->setSectionResizeMode(COL_DURATION, QHeaderView::Interactive);

  // we set up the name column first, EID second, so that the name column gets the
  // expand/collapse widgets. Then we need to put them back in order
  ui->events->header()->moveSection(COL_NAME, COL_EID);

  // Qt doesn't allow moving the column with the expand/collapse widgets, so this
  // becomes quickly infuriating to rearrange, just disable until that can be fixed.
  ui->events->header()->setSectionsMovable(false);

  m_SizeDelegate = new SizeDelegate(QSize(0, 16));
  ui->events->setItemDelegate(m_SizeDelegate);

  UpdateDurationColumn();

  m_FindHighlight = new QTimer(this);
  m_FindHighlight->setInterval(400);
  m_FindHighlight->setSingleShot(true);
  connect(m_FindHighlight, &QTimer::timeout, this, &EventBrowser::findHighlight_timeout);

  QObject::connect(ui->closeFind, &QToolButton::clicked, this, &EventBrowser::on_HideFindJump);
  QObject::connect(ui->closeJump, &QToolButton::clicked, this, &EventBrowser::on_HideFindJump);
  QObject::connect(ui->events, &RDTreeWidget::keyPress, this, &EventBrowser::events_keyPress);
  ui->jumpStrip->hide();
  ui->findStrip->hide();
  ui->bookmarkStrip->hide();

  m_BookmarkStripLayout = new FlowLayout(ui->bookmarkStrip, 0, 3, 3);
  m_BookmarkSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

  ui->bookmarkStrip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  m_BookmarkStripLayout->addWidget(ui->bookmarkStripHeader);
  m_BookmarkStripLayout->addItem(m_BookmarkSpacer);

  Qt::Key keys[] = {
      Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5,
      Qt::Key_6, Qt::Key_7, Qt::Key_8, Qt::Key_9, Qt::Key_0,
  };
  for(int i = 0; i < 10; i++)
  {
    QShortcut *sc = new QShortcut(QKeySequence(keys[i] | Qt::ControlModifier), this);
    QObject::connect(sc, &QShortcut::activated, [this, i]() { jumpToBookmark(i); });
  }

  {
    QShortcut *sc = new QShortcut(QKeySequence(Qt::Key_Left | Qt::ControlModifier), this);
    QObject::connect(sc, &QShortcut::activated, this, &EventBrowser::on_stepPrev_clicked);
  }

  {
    QShortcut *sc = new QShortcut(QKeySequence(Qt::Key_Right | Qt::ControlModifier), this);
    QObject::connect(sc, &QShortcut::activated, this, &EventBrowser::on_stepNext_clicked);
  }

  ui->events->setContextMenuPolicy(Qt::CustomContextMenu);
  QObject::connect(ui->events, &RDTreeWidget::customContextMenuRequested, this,
                   &EventBrowser::events_contextMenu);

  OnLogfileClosed();
}

EventBrowser::~EventBrowser()
{
  m_Ctx.BuiltinWindowClosed(this);
  m_Ctx.RemoveLogViewer(this);
  delete ui;
  delete m_SizeDelegate;
}

void EventBrowser::OnLogfileLoaded()
{
  RDTreeWidgetItem *frame = new RDTreeWidgetItem(
      {QFormatStr("Frame #%1").arg(m_Ctx.FrameInfo().frameNumber), QString(), QString()});

  clearBookmarks();

  RDTreeWidgetItem *framestart = new RDTreeWidgetItem({tr("Frame Start"), lit("0"), QString()});
  framestart->setTag(QVariant::fromValue(EventItemTag()));

  frame->addChild(framestart);

  uint lastEID = AddDrawcalls(frame, m_Ctx.CurDrawcalls());
  frame->setTag(QVariant::fromValue(EventItemTag(0, lastEID)));

  ui->events->addTopLevelItem(frame);

  ui->events->expandItem(frame);

  ui->find->setEnabled(true);
  ui->gotoEID->setEnabled(true);
  ui->timeDraws->setEnabled(true);
  ui->bookmark->setEnabled(true);
  ui->exportDraws->setEnabled(true);
  ui->stepPrev->setEnabled(true);
  ui->stepNext->setEnabled(true);

  m_Ctx.SetEventID({this}, lastEID, lastEID);
}

void EventBrowser::OnLogfileClosed()
{
  clearBookmarks();

  ui->events->clear();

  ui->find->setEnabled(false);
  ui->gotoEID->setEnabled(false);
  ui->timeDraws->setEnabled(false);
  ui->bookmark->setEnabled(false);
  ui->exportDraws->setEnabled(false);
  ui->stepPrev->setEnabled(false);
  ui->stepNext->setEnabled(false);
}

void EventBrowser::OnEventChanged(uint32_t eventID)
{
  SelectEvent(eventID);
  highlightBookmarks();
}

uint EventBrowser::AddDrawcalls(RDTreeWidgetItem *parent,
                                const rdctype::array<DrawcallDescription> &draws)
{
  uint lastEID = 0;

  for(int32_t i = 0; i < draws.count; i++)
  {
    const DrawcallDescription &d = draws[i];

    RDTreeWidgetItem *child =
        new RDTreeWidgetItem({ToQStr(d.name), QFormatStr("%1").arg(d.eventID), lit("0.0")});

    lastEID = AddDrawcalls(child, d.children);

    if(lastEID > d.eventID)
      child->setText(COL_EID, QFormatStr("%1-%2").arg(d.eventID).arg(lastEID));

    if(lastEID == 0)
    {
      lastEID = d.eventID;

      if((draws[i].flags & DrawFlags::SetMarker) && i + 1 < draws.count)
        lastEID = draws[i + 1].eventID;
    }

    child->setTag(QVariant::fromValue(EventItemTag(draws[i].eventID, lastEID)));

    if(m_Ctx.Config().EventBrowser_ApplyColors)
    {
      // if alpha isn't 0, assume the colour is valid
      if((d.flags & (DrawFlags::PushMarker | DrawFlags::SetMarker)) && d.markerColor[3] > 0.0f)
      {
        QColor col = QColor::fromRgb(
            qRgb(d.markerColor[0] * 255.0f, d.markerColor[1] * 255.0f, d.markerColor[2] * 255.0f));

        child->setTreeColor(col, 3.0f);

        if(m_Ctx.Config().EventBrowser_ColorEventRow)
        {
          QColor textCol = ui->events->palette().color(QPalette::Text);

          child->setBackgroundColor(col);
          child->setForegroundColor(contrastingColor(col, textCol));
        }
      }
    }

    parent->addChild(child);
  }

  return lastEID;
}

void EventBrowser::SetDrawcallTimes(RDTreeWidgetItem *node,
                                    const rdctype::array<CounterResult> &results)
{
  if(node == NULL)
    return;

  // parent nodes take the value of the sum of their children
  double duration = 0.0;

  // look up leaf nodes in the dictionary
  if(node->childCount() == 0)
  {
    uint32_t eid = node->tag().value<EventItemTag>().EID;

    duration = -1.0;

    for(const CounterResult &r : results)
    {
      if(r.eventID == eid)
        duration = r.value.d;
    }

    double secs = duration;

    if(m_TimeUnit == TimeUnit::Milliseconds)
      secs *= 1000.0;
    else if(m_TimeUnit == TimeUnit::Microseconds)
      secs *= 1000000.0;
    else if(m_TimeUnit == TimeUnit::Nanoseconds)
      secs *= 1000000000.0;

    node->setText(COL_DURATION, duration < 0.0f ? QString() : QString::number(secs));
    EventItemTag tag = node->tag().value<EventItemTag>();
    tag.duration = duration;
    node->setTag(QVariant::fromValue(tag));

    return;
  }

  for(int i = 0; i < node->childCount(); i++)
  {
    SetDrawcallTimes(node->child(i), results);

    double nd = node->tag().value<EventItemTag>().duration;

    if(nd > 0.0)
      duration += nd;
  }

  double secs = duration;

  if(m_TimeUnit == TimeUnit::Milliseconds)
    secs *= 1000.0;
  else if(m_TimeUnit == TimeUnit::Microseconds)
    secs *= 1000000.0;
  else if(m_TimeUnit == TimeUnit::Nanoseconds)
    secs *= 1000000000.0;

  node->setText(COL_DURATION, duration < 0.0f ? QString() : QString::number(secs));
  EventItemTag tag = node->tag().value<EventItemTag>();
  tag.duration = duration;
  node->setTag(QVariant::fromValue(tag));
}

void EventBrowser::on_find_clicked()
{
  ui->jumpStrip->hide();
  ui->findStrip->show();
  ui->findEvent->setFocus();
}

void EventBrowser::on_gotoEID_clicked()
{
  ui->jumpStrip->show();
  ui->findStrip->hide();
  ui->jumpToEID->setFocus();
}

void EventBrowser::on_bookmark_clicked()
{
  RDTreeWidgetItem *n = ui->events->currentItem();

  if(n)
    toggleBookmark(n->tag().value<EventItemTag>().lastEID);
}

void EventBrowser::on_timeDraws_clicked()
{
  m_Ctx.Replay().AsyncInvoke([this](IReplayController *r) {

    m_Times = r->FetchCounters({GPUCounter::EventGPUDuration});

    GUIInvoke::call([this]() { SetDrawcallTimes(ui->events->topLevelItem(0), m_Times); });
  });
}

void EventBrowser::on_events_currentItemChanged(RDTreeWidgetItem *current, RDTreeWidgetItem *previous)
{
  if(previous)
  {
    EventItemTag tag = previous->tag().value<EventItemTag>();
    tag.current = false;
    previous->setTag(QVariant::fromValue(tag));
    RefreshIcon(previous, tag);
  }

  if(!current)
    return;

  EventItemTag tag = current->tag().value<EventItemTag>();
  tag.current = true;
  current->setTag(QVariant::fromValue(tag));
  RefreshIcon(current, tag);

  m_Ctx.SetEventID({this}, tag.EID, tag.lastEID);

  highlightBookmarks();
}

void EventBrowser::on_HideFindJump()
{
  ui->jumpStrip->hide();
  ui->findStrip->hide();

  ui->jumpToEID->setText(QString());

  ClearFindIcons();
  ui->findEvent->setStyleSheet(QString());
}

void EventBrowser::on_jumpToEID_returnPressed()
{
  bool ok = false;
  uint eid = ui->jumpToEID->text().toUInt(&ok);
  if(ok)
  {
    SelectEvent(eid);
  }
}

void EventBrowser::findHighlight_timeout()
{
  ClearFindIcons();

  int results = SetFindIcons(ui->findEvent->text());

  if(results > 0)
    ui->findEvent->setStyleSheet(QString());
  else
    ui->findEvent->setStyleSheet(lit("QLineEdit{background-color:#ff0000;}"));
}

void EventBrowser::on_findEvent_textEdited(const QString &arg1)
{
  if(arg1.isEmpty())
  {
    m_FindHighlight->stop();

    ui->findEvent->setStyleSheet(QString());
    ClearFindIcons();
  }
  else
  {
    m_FindHighlight->start();    // restart
  }
}

void EventBrowser::on_findEvent_returnPressed()
{
  // stop the timer, we'll manually fire it instantly
  if(m_FindHighlight->isActive())
    m_FindHighlight->stop();

  if(!ui->findEvent->text().isEmpty())
    Find(true);

  findHighlight_timeout();
}

void EventBrowser::on_findEvent_keyPress(QKeyEvent *event)
{
  if(event->key() == Qt::Key_F3)
  {
    // stop the timer, we'll manually fire it instantly
    if(m_FindHighlight->isActive())
      m_FindHighlight->stop();

    if(!ui->findEvent->text().isEmpty())
      Find(event->modifiers() & Qt::ShiftModifier ? false : true);

    findHighlight_timeout();

    event->accept();
  }
}

void EventBrowser::on_findNext_clicked()
{
  Find(true);
}

void EventBrowser::on_findPrev_clicked()
{
  Find(false);
}

void EventBrowser::on_stepNext_clicked()
{
  if(!m_Ctx.LogLoaded())
    return;

  const DrawcallDescription *draw = m_Ctx.CurDrawcall();

  if(draw && draw->next > 0)
    SelectEvent(draw->next);
}

void EventBrowser::on_stepPrev_clicked()
{
  if(!m_Ctx.LogLoaded())
    return;

  const DrawcallDescription *draw = m_Ctx.CurDrawcall();

  if(draw && draw->previous > 0)
    SelectEvent(draw->previous);
}

void EventBrowser::on_exportDraws_clicked()
{
  QString filename =
      RDDialog::getSaveFileName(this, tr("Save Event List"), QString(), lit("Text files (*.txt)"));

  if(!filename.isEmpty())
  {
    QDir dirinfo = QFileInfo(filename).dir();
    if(dirinfo.exists())
    {
      QFile f(filename);
      if(f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      {
        QTextStream stream(&f);

        stream << tr("%1 - Frame #%2\n\n").arg(m_Ctx.LogFilename()).arg(m_Ctx.FrameInfo().frameNumber);

        int maxNameLength = 0;

        for(const DrawcallDescription &d : m_Ctx.CurDrawcalls())
          GetMaxNameLength(maxNameLength, 0, false, d);

        QString line = QFormatStr(" EID  | %1 | Draw #").arg(lit("Event"), -maxNameLength);

        if(!m_Times.empty())
        {
          line += QFormatStr(" | %1").arg(ui->events->headerText(COL_DURATION));
        }

        stream << line << "\n";

        line = QFormatStr("--------%1-----------").arg(QString(), maxNameLength, QLatin1Char('-'));

        if(!m_Times.empty())
        {
          int maxDurationLength = 0;
          maxDurationLength = qMax(maxDurationLength, Formatter::Format(1.0).length());
          maxDurationLength = qMax(maxDurationLength, Formatter::Format(1.2345e-200).length());
          maxDurationLength =
              qMax(maxDurationLength, Formatter::Format(123456.7890123456789).length());
          line += QString(3 + maxDurationLength, QLatin1Char('-'));    // 3 extra for " | "
        }

        stream << line << "\n";

        for(const DrawcallDescription &d : m_Ctx.CurDrawcalls())
          ExportDrawcall(stream, maxNameLength, 0, false, d);
      }
      else
      {
        RDDialog::critical(
            this, tr("Error saving event list"),
            tr("Couldn't open path %1 for write.\n%2").arg(filename).arg(f.errorString()));
        return;
      }
    }
    else
    {
      RDDialog::critical(this, tr("Invalid directory"),
                         tr("Cannot find target directory to save to"));
      return;
    }
  }
}

QString EventBrowser::GetExportDrawcallString(int indent, bool firstchild,
                                              const DrawcallDescription &drawcall)
{
  QString prefix = QString(indent * 2 - (firstchild ? 1 : 0), QLatin1Char(' '));
  if(firstchild)
    prefix += QLatin1Char('\\');

  return QFormatStr("%1- %2").arg(prefix).arg(ToQStr(drawcall.name));
}

double EventBrowser::GetDrawTime(const DrawcallDescription &drawcall)
{
  if(!drawcall.children.empty())
  {
    double total = 0.0;

    for(const DrawcallDescription &d : drawcall.children)
    {
      double f = GetDrawTime(d);
      if(f >= 0)
        total += f;
    }

    return total;
  }

  for(const CounterResult &r : m_Times)
  {
    if(r.eventID == drawcall.eventID)
      return r.value.d;
  }

  return -1.0;
}

void EventBrowser::GetMaxNameLength(int &maxNameLength, int indent, bool firstchild,
                                    const DrawcallDescription &drawcall)
{
  QString nameString = GetExportDrawcallString(indent, firstchild, drawcall);

  maxNameLength = qMax(maxNameLength, nameString.count());

  firstchild = true;

  for(const DrawcallDescription &d : drawcall.children)
  {
    GetMaxNameLength(maxNameLength, indent + 1, firstchild, d);
    firstchild = false;
  }
}

void EventBrowser::ExportDrawcall(QTextStream &writer, int maxNameLength, int indent,
                                  bool firstchild, const DrawcallDescription &drawcall)
{
  QString eidString = drawcall.children.empty() ? QString::number(drawcall.eventID) : QString();

  QString nameString = GetExportDrawcallString(indent, firstchild, drawcall);

  QString line = QFormatStr("%1 | %2 | %3")
                     .arg(eidString, -5)
                     .arg(nameString, -maxNameLength)
                     .arg(drawcall.drawcallID, -6);

  if(!m_Times.empty())
  {
    double f = GetDrawTime(drawcall);

    if(f >= 0)
    {
      if(m_TimeUnit == TimeUnit::Milliseconds)
        f *= 1000.0;
      else if(m_TimeUnit == TimeUnit::Microseconds)
        f *= 1000000.0;
      else if(m_TimeUnit == TimeUnit::Nanoseconds)
        f *= 1000000000.0;

      line += QFormatStr(" | %1").arg(Formatter::Format(f));
    }
    else
    {
      line += lit(" |");
    }
  }

  writer << line << "\n";

  firstchild = true;

  for(const DrawcallDescription &d : drawcall.children)
  {
    ExportDrawcall(writer, maxNameLength, indent + 1, firstchild, d);
    firstchild = false;
  }
}

void EventBrowser::events_keyPress(QKeyEvent *event)
{
  if(!m_Ctx.LogLoaded())
    return;

  if(event->key() == Qt::Key_F3)
  {
    if(event->modifiers() == Qt::ShiftModifier)
      Find(false);
    else
      Find(true);
  }

  if(event->modifiers() == Qt::ControlModifier)
  {
    if(event->key() == Qt::Key_F)
    {
      on_find_clicked();
      event->accept();
    }
    else if(event->key() == Qt::Key_G)
    {
      on_gotoEID_clicked();
      event->accept();
    }
    else if(event->key() == Qt::Key_B)
    {
      on_bookmark_clicked();
      event->accept();
    }
    else if(event->key() == Qt::Key_T)
    {
      on_timeDraws_clicked();
      event->accept();
    }
  }
}

void EventBrowser::events_contextMenu(const QPoint &pos)
{
  if(!m_Ctx.LogLoaded())
    return;

  RDTreeWidgetItem *item = ui->events->itemAt(pos);

  if(item)
  {
    QMenu contextMenu(this);

    QAction expandAll(tr("Expand All"), this);
    QAction collapseAll(tr("Collapse All"), this);

    contextMenu.addAction(&expandAll);
    contextMenu.addAction(&collapseAll);

    expandAll.setIcon(Icons::fit_window());
    collapseAll.setIcon(Icons::arrow_in());

    expandAll.setEnabled(item->childCount() > 0);
    collapseAll.setEnabled(item->childCount() > 0);

    QObject::connect(&expandAll, &QAction::triggered,
                     [this, item]() { ui->events->expandAllItems(item); });

    QObject::connect(&collapseAll, &QAction::triggered,
                     [this, item]() { ui->events->collapseAllItems(item); });

    RDDialog::show(&contextMenu, ui->events->viewport()->mapToGlobal(pos));
  }
}

void EventBrowser::clearBookmarks()
{
  for(QToolButton *b : m_BookmarkButtons)
    delete b;

  m_Bookmarks.clear();
  m_BookmarkButtons.clear();

  ui->bookmarkStrip->setVisible(false);
}

void EventBrowser::toggleBookmark(uint32_t EID)
{
  int index = m_Bookmarks.indexOf(EID);

  RDTreeWidgetItem *found = NULL;
  FindEventNode(found, ui->events->topLevelItem(0), EID);

  if(index >= 0)
  {
    delete m_BookmarkButtons.takeAt(index);
    m_Bookmarks.removeAt(index);

    if(found)
    {
      EventItemTag tag = found->tag().value<EventItemTag>();
      tag.bookmark = false;
      found->setTag(QVariant::fromValue(tag));
      RefreshIcon(found, tag);
    }
  }
  else
  {
    QToolButton *but = new QToolButton(this);

    but->setText(QString::number(EID));
    but->setCheckable(true);
    but->setAutoRaise(true);
    but->setProperty("eid", EID);
    QObject::connect(but, &QToolButton::clicked, [this, but, EID]() {
      but->setChecked(true);
      SelectEvent(EID);
      highlightBookmarks();
    });

    m_BookmarkButtons.push_back(but);
    m_Bookmarks.push_back(EID);

    highlightBookmarks();

    if(found)
    {
      EventItemTag tag = found->tag().value<EventItemTag>();
      tag.bookmark = true;
      found->setTag(QVariant::fromValue(tag));
      RefreshIcon(found, tag);
    }

    m_BookmarkStripLayout->removeItem(m_BookmarkSpacer);
    m_BookmarkStripLayout->addWidget(but);
    m_BookmarkStripLayout->addItem(m_BookmarkSpacer);
  }

  ui->bookmarkStrip->setVisible(!m_BookmarkButtons.isEmpty());
}

void EventBrowser::jumpToBookmark(int idx)
{
  if(idx < 0 || idx >= m_Bookmarks.count() || !m_Ctx.LogLoaded())
    return;

  // don't exclude ourselves, so we're updated as normal
  SelectEvent(m_Bookmarks[idx]);
}

void EventBrowser::highlightBookmarks()
{
  for(QToolButton *b : m_BookmarkButtons)
  {
    if(b->property("eid").toUInt() == m_Ctx.CurEvent())
      b->setChecked(true);
    else
      b->setChecked(false);
  }
}

bool EventBrowser::hasBookmark(RDTreeWidgetItem *node)
{
  if(node)
    return hasBookmark(node->tag().value<EventItemTag>().EID);

  return false;
}

bool EventBrowser::hasBookmark(uint32_t EID)
{
  return m_Bookmarks.contains(EID);
}

void EventBrowser::RefreshIcon(RDTreeWidgetItem *item, EventItemTag tag)
{
  if(tag.current)
    item->setIcon(COL_NAME, Icons::flag_green());
  else if(tag.bookmark)
    item->setIcon(COL_NAME, Icons::asterisk_orange());
  else if(tag.find)
    item->setIcon(COL_NAME, Icons::find());
  else
    item->setIcon(COL_NAME, QIcon());
}

bool EventBrowser::FindEventNode(RDTreeWidgetItem *&found, RDTreeWidgetItem *parent, uint32_t eventID)
{
  // do a reverse search to find the last match (in case of 'set' markers that
  // inherit the event of the next real draw).
  for(int i = parent->childCount() - 1; i >= 0; i--)
  {
    RDTreeWidgetItem *n = parent->child(i);

    uint nEID = n->tag().value<EventItemTag>().lastEID;
    uint fEID = found ? found->tag().value<EventItemTag>().lastEID : 0;

    if(nEID >= eventID && (found == NULL || nEID <= fEID))
      found = n;

    if(nEID == eventID && n->childCount() == 0)
      return true;

    if(n->childCount() > 0)
    {
      bool exact = FindEventNode(found, n, eventID);
      if(exact)
        return true;
    }
  }

  return false;
}

void EventBrowser::ExpandNode(RDTreeWidgetItem *node)
{
  RDTreeWidgetItem *n = node;
  while(node != NULL)
  {
    ui->events->expandItem(node);
    node = node->parent();
  }

  if(n)
    ui->events->scrollToItem(n);
}

bool EventBrowser::SelectEvent(uint32_t eventID)
{
  if(!m_Ctx.LogLoaded())
    return false;

  RDTreeWidgetItem *found = NULL;
  FindEventNode(found, ui->events->topLevelItem(0), eventID);
  if(found != NULL)
  {
    ui->events->setCurrentItem(found);
    ui->events->setSelectedItem(found);

    ExpandNode(found);
    return true;
  }

  return false;
}

void EventBrowser::ClearFindIcons(RDTreeWidgetItem *parent)
{
  for(int i = 0; i < parent->childCount(); i++)
  {
    RDTreeWidgetItem *n = parent->child(i);

    EventItemTag tag = n->tag().value<EventItemTag>();
    tag.find = false;
    n->setTag(QVariant::fromValue(tag));
    RefreshIcon(n, tag);

    if(n->childCount() > 0)
      ClearFindIcons(n);
  }
}

void EventBrowser::ClearFindIcons()
{
  if(m_Ctx.LogLoaded())
    ClearFindIcons(ui->events->topLevelItem(0));
}

int EventBrowser::SetFindIcons(RDTreeWidgetItem *parent, QString filter)
{
  int results = 0;

  for(int i = 0; i < parent->childCount(); i++)
  {
    RDTreeWidgetItem *n = parent->child(i);

    if(n->text(COL_NAME).contains(filter, Qt::CaseInsensitive))
    {
      EventItemTag tag = n->tag().value<EventItemTag>();
      tag.find = true;
      n->setTag(QVariant::fromValue(tag));
      RefreshIcon(n, tag);
      results++;
    }

    if(n->childCount() > 0)
    {
      results += SetFindIcons(n, filter);
    }
  }

  return results;
}

int EventBrowser::SetFindIcons(QString filter)
{
  if(filter.isEmpty())
    return 0;

  return SetFindIcons(ui->events->topLevelItem(0), filter);
}

RDTreeWidgetItem *EventBrowser::FindNode(RDTreeWidgetItem *parent, QString filter, uint32_t after)
{
  for(int i = 0; i < parent->childCount(); i++)
  {
    RDTreeWidgetItem *n = parent->child(i);

    uint eid = n->tag().value<EventItemTag>().lastEID;

    if(eid > after && n->text(COL_NAME).contains(filter, Qt::CaseInsensitive))
      return n;

    if(n->childCount() > 0)
    {
      RDTreeWidgetItem *found = FindNode(n, filter, after);

      if(found != NULL)
        return found;
    }
  }

  return NULL;
}

int EventBrowser::FindEvent(RDTreeWidgetItem *parent, QString filter, uint32_t after, bool forward)
{
  if(parent == NULL)
    return -1;

  for(int i = forward ? 0 : parent->childCount() - 1; i >= 0 && i < parent->childCount();
      i += forward ? 1 : -1)
  {
    auto n = parent->child(i);

    uint eid = n->tag().value<EventItemTag>().lastEID;

    bool matchesAfter = (forward && eid > after) || (!forward && eid < after);

    if(matchesAfter)
    {
      QString name = n->text(COL_NAME);
      if(name.contains(filter, Qt::CaseInsensitive))
        return (int)eid;
    }

    if(n->childCount() > 0)
    {
      int found = FindEvent(n, filter, after, forward);

      if(found > 0)
        return found;
    }
  }

  return -1;
}

int EventBrowser::FindEvent(QString filter, uint32_t after, bool forward)
{
  if(!m_Ctx.LogLoaded())
    return 0;

  return FindEvent(ui->events->topLevelItem(0), filter, after, forward);
}

void EventBrowser::Find(bool forward)
{
  if(ui->findEvent->text().isEmpty())
    return;

  uint32_t curEID = m_Ctx.CurEvent();

  RDTreeWidgetItem *node = ui->events->selectedItem();
  if(node)
    curEID = node->tag().value<EventItemTag>().lastEID;

  int eid = FindEvent(ui->findEvent->text(), curEID, forward);
  if(eid >= 0)
  {
    SelectEvent((uint32_t)eid);
    ui->findEvent->setStyleSheet(QString());
  }
  else    // if(WrapSearch)
  {
    eid = FindEvent(ui->findEvent->text(), forward ? 0 : ~0U, forward);
    if(eid >= 0)
    {
      SelectEvent((uint32_t)eid);
      ui->findEvent->setStyleSheet(QString());
    }
    else
    {
      ui->findEvent->setStyleSheet(lit("QLineEdit{background-color:#ff0000;}"));
    }
  }
}

void EventBrowser::UpdateDurationColumn()
{
  if(m_TimeUnit == m_Ctx.Config().EventBrowser_TimeUnit)
    return;

  m_TimeUnit = m_Ctx.Config().EventBrowser_TimeUnit;

  ui->events->setHeaderText(COL_DURATION, tr("Duration (%1)").arg(UnitSuffix(m_TimeUnit)));

  if(!m_Times.empty())
    SetDrawcallTimes(ui->events->topLevelItem(0), m_Times);
}
