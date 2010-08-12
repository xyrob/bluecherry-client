#include "EventTimelineWidget.h"
#include "EventsModel.h"
#include "core/DVRServer.h"
#include <QPaintEvent>
#include <QPainter>
#include <QVector>
#include <QScrollBar>
#include <QFontMetrics>
#include <QDebug>
#include <qmath.h>

struct LocationData
{
    ServerData *serverData;
    QString location;
    QList<EventData*> events;
};

struct ServerData
{
    DVRServer *server;
    QHash<QString,LocationData*> locationsMap;
};

EventTimelineWidget::EventTimelineWidget(QWidget *parent)
    : QAbstractItemView(parent), timeSeconds(0), viewSeconds(0), primaryTickSecs(0), cachedTopPadding(0)
{
    setFrameStyle(QFrame::NoFrame);
    setAutoFillBackground(false);

    connect(horizontalScrollBar(), SIGNAL(valueChanged(int)), SLOT(setViewStartOffset(int)));
}

EventTimelineWidget::~EventTimelineWidget()
{
    clearData();
}

void EventTimelineWidget::clearData()
{
    foreach (ServerData *server, serversMap)
    {
        qDeleteAll(server->locationsMap);
        delete server;
    }

    serversMap.clear();
    rowsMap.clear();
    timeStart = timeEnd = QDateTime();
    viewTimeStart = viewTimeEnd = QDateTime();
    timeSeconds = viewSeconds = 0;
    viewport()->update();

    emit zoomRangeChanged(0, 0);
    emit zoomSecondsChanged(0);
}

void EventTimelineWidget::setModel(QAbstractItemModel *newModel)
{
    if (newModel == model())
        return;

    if (model())
    {
        model()->disconnect(this);
        clearData();
    }

    connect(newModel, SIGNAL(rowsRemoved(QModelIndex,int,int)), SLOT(rowsRemoved(QModelIndex,int,int)));
    connect(newModel, SIGNAL(modelReset()), SLOT(modelReset()));
    QAbstractItemView::setModel(newModel);

    addModelRows(0);
}

QRect EventTimelineWidget::visualRect(const QModelIndex &index) const
{
    if (!index.isValid())
        return QRect();

    EventData *event = rowData(index.row());
    if (!event)
        return QRect();

    ServerData *serverData;
    LocationData *locationData;
    if (!const_cast<EventTimelineWidget*>(this)->findEvent(event, false, &serverData, &locationData, 0))
        return QRect();

    QRect itemArea = viewportItemArea();
    int y = itemArea.top();
    for (QHash<DVRServer*,ServerData*>::ConstIterator it = serversMap.begin(); it != serversMap.end(); ++it)
    {
        y += rowHeight();
        if (*it != serverData)
        {
            y += rowHeight() * (*it)->locationsMap.size();
            continue;
        }

        for (QHash<QString,LocationData*>::ConstIterator locit = serverData->locationsMap.begin();
             locit != serverData->locationsMap.end(); ++locit)
        {
            if (*locit == locationData)
                break;
            y += rowHeight();
        }

        QRect re = timeCellRect(event->date, event->duration);
        re.translate(itemArea.topLeft());
        re.moveTop(y);
        re.setHeight(rowHeight());

        return re;
    }

    return QRect();
}

double EventTimelineWidget::zoomLevel() const
{
    /* Zoom level of 0 indicates that the entire span of time (timeStart to timeEnd) is
     * visible; timeStart/viewTimeStart and timeEnd/viewTimeEnd are equal. From there, the
     * span (by number of seconds) is scaled up to 100, with 100 indicating maximum zoom,
     * which we define as 1 minute for simplicity. In other words, we scale viewSeconds
     * between 60 and timeSeconds and use the inverse. */
    if (viewSeconds == timeSeconds)
        return 0;
    return 100-((double(viewSeconds-minZoomSeconds())/double(timeSeconds-minZoomSeconds()))*100);
}

void EventTimelineWidget::setZoomLevel(double level)
{
    level = (100 - qBound(0.0, level, 100.0))/100.0;
    int seconds = qRound((level*timeSeconds)+minZoomSeconds());
    setZoomSeconds(seconds);
}

int EventTimelineWidget::zoomSeconds() const
{
    return viewSeconds;
}

void EventTimelineWidget::setZoomSeconds(int seconds)
{
    seconds = qBound(minZoomSeconds(), seconds, maxZoomSeconds());
    if (viewSeconds == seconds)
        return;

    Q_ASSERT(!viewTimeStart.isNull());
    Q_ASSERT(viewTimeStart >= timeStart);

    viewSeconds = seconds;
    viewTimeEnd = viewTimeStart.addSecs(seconds);
    if (viewTimeEnd > timeEnd)
    {
        viewTimeStart = viewTimeStart.addSecs(viewTimeEnd.secsTo(timeEnd));
        viewTimeEnd = timeEnd;
    }

    Q_ASSERT(viewTimeEnd > viewTimeStart);
    Q_ASSERT(viewTimeStart >= timeStart);
    Q_ASSERT(viewTimeEnd <= timeEnd);
    Q_ASSERT(viewTimeStart.secsTo(viewTimeEnd) == viewSeconds);

    updateTimeRange(false);
}

void EventTimelineWidget::setViewStartOffset(int secs)
{
    secs = qBound(0, secs, timeSeconds - viewSeconds);

    viewTimeStart = timeStart.addSecs(secs);
    viewTimeEnd = viewTimeStart.addSecs(viewSeconds);

    Q_ASSERT(viewSeconds <= timeSeconds);
    Q_ASSERT(viewSeconds >= minZoomSeconds());
    Q_ASSERT(viewTimeEnd <= timeEnd);
    Q_ASSERT(horizontalScrollBar()->maximum() == (timeSeconds - viewSeconds));

    viewport()->update();
}

void EventTimelineWidget::updateScrollBars()
{
    horizontalScrollBar()->setRange(0, qMax(timeSeconds-viewSeconds, 0));
    horizontalScrollBar()->setPageStep(primaryTickSecs);
    horizontalScrollBar()->setSingleStep(horizontalScrollBar()->pageStep());
}

void EventTimelineWidget::scrollTo(const QModelIndex &index, ScrollHint hint)
{

}

EventData *EventTimelineWidget::eventAt(const QPoint &point) const
{
    QRect itemArea = viewportItemArea();
    if (!itemArea.contains(point))
        return 0;

    /* Iterate servers and locations to the specified y coordinate */
    int y = itemArea.top();
    int n = (point.y()-y)/rowHeight();

    for (QHash<DVRServer*,ServerData*>::ConstIterator it = serversMap.begin(); it != serversMap.end(); ++it)
    {
        if (--n < 0)
            return 0;

        int count = (*it)->locationsMap.size();
        if (n >= count)
        {
            n -= count;
            continue;
        }

        LocationData *location = *((*it)->locationsMap.begin() + n);

        /* This is slow and can likely be improved. */
        for (QList<EventData*>::ConstIterator evit = location->events.begin(); evit != location->events.end(); ++evit)
        {
            QRect eventRect = timeCellRect((*evit)->date, (*evit)->duration).translated(itemArea.left(), 0);
            if (point.x() >= eventRect.left() && point.x() <= eventRect.right())
                return *evit;
        }

        break;
    }

    return 0;
}

QModelIndex EventTimelineWidget::indexAt(const QPoint &point) const
{
    EventData *event = eventAt(point);
    if (!event)
        return QModelIndex();

    int row = rowsMap[event];
    return model()->index(row, 0);
}

QSize EventTimelineWidget::sizeHint() const
{
    return QSize(500, 300);
}

QModelIndex EventTimelineWidget::moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers)
{
    return QModelIndex();
}

bool EventTimelineWidget::isIndexHidden(const QModelIndex &index) const
{
    return false;
}

int EventTimelineWidget::horizontalOffset() const
{
    return 0;
}

int EventTimelineWidget::verticalOffset() const
{
    return 0;
}

void EventTimelineWidget::setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags command)
{

}

QRegion EventTimelineWidget::visualRegionForSelection(const QItemSelection &selection) const
{
    return QRegion();
}

EventData *EventTimelineWidget::rowData(int row) const
{
    QModelIndex idx = model()->index(row, 0);
    return idx.data(EventsModel::EventDataPtr).value<EventData*>();
}

bool EventTimelineWidget::findEvent(EventData *event, bool create, ServerData **server,
                                    LocationData **location, int *position)
{
    if (server)
        *server = 0;
    if (location)
        *location = 0;
    if (position)
        *position = -1;

    /* Find associated server */
    QHash<DVRServer*,ServerData*>::ConstIterator it = serversMap.find(event->server);
    if (it == serversMap.end())
    {
        if (!create)
            return false;

        ServerData *serverData = new ServerData;
        serverData->server = event->server;
        it = serversMap.insert(serverData->server, serverData);
    }

    ServerData *serverData = *it;
    if (server)
        *server = serverData;

    /* Find associated location (within the server) */
    QHash<QString,LocationData*>::ConstIterator lit = serverData->locationsMap.find(event->location);
    if (lit == serverData->locationsMap.end())
    {
        if (!create)
            return false;

        LocationData *locationData = new LocationData;
        locationData->location = event->location;
        locationData->serverData = serverData;
        lit = serverData->locationsMap.insert(locationData->location, locationData);
    }

    LocationData *locationData = *lit;
    if (location)
        *location = locationData;

    if (position && create)
    {
        /* Find the position where this event belongs */
        int p = 0;
        for (int n = locationData->events.size(); p < n; ++p)
        {
            if (event->date < locationData->events[p]->date)
                break;
        }
        *position = p;
    }
    else if (position)
        *position = locationData->events.indexOf(event);

    return true;
}

void EventTimelineWidget::updateTimeRange(bool fromData)
{
    if (fromData)
    {
        /* Refresh dataTimeStart and dataTimeEnd */
        dataTimeStart = dataTimeEnd = QDateTime();

        for (QHash<EventData*,int>::Iterator it = rowsMap.begin(); it != rowsMap.end(); ++it)
        {
            QDateTime date = it.key()->date;
            if (dataTimeStart.isNull() || date < dataTimeStart)
                dataTimeStart = date;
            date = date.addSecs(qMax(it.key()->duration,1));
            if (dataTimeEnd.isNull() || date > dataTimeEnd)
                dataTimeEnd = date;
        }
    }

    /* Approximate viewSeconds for the tick calculations */
    if (viewTimeStart.isNull() || viewTimeEnd.isNull())
        viewSeconds = dataTimeStart.secsTo(dataTimeEnd);
    else
        viewSeconds = qMin(viewSeconds, dataTimeStart.secsTo(dataTimeEnd));

    /* Determine the minimum width for the primary tick (the tick with a label),
     * which is then used to determine its interval. */
    QFontMetrics fm(font());
    int minTickWidth = qMax(fm.width(tr("22:22"))+6, 16);

    /* Using the minimum tick width, find the minimum number of seconds per tick,
     * and round up to an even and user-friendly duration */
    int areaWidth = viewportItemArea().width();
    int minTickSecs = qMax(int(viewSeconds / (double(areaWidth) / minTickWidth)), 1);

    if (minTickSecs <= 30)
        primaryTickSecs = 30;
    else if (minTickSecs <= 60)
        primaryTickSecs = 60;
    else if (minTickSecs <= 300)
        primaryTickSecs = 300;
    else if (minTickSecs <= 600)
        primaryTickSecs = 600;
    else if (minTickSecs <= 3600)
        primaryTickSecs = 3600;
    else if (minTickSecs <= 7200)
        primaryTickSecs = 7200;
    else if (minTickSecs <= 21600)
        primaryTickSecs = 21600;
    else if (minTickSecs <= 43200)
        primaryTickSecs = 43200;
    else if (minTickSecs <= 86400)
        primaryTickSecs = 86400;
    else
        primaryTickSecs = 604800;

    /* Set timeStart and timeEnd to rounded values of primaryTickSecs */
    timeStart = dataTimeStart.addSecs(-int(dataTimeStart.toTime_t()%primaryTickSecs));
    timeEnd = dataTimeEnd.addSecs(primaryTickSecs-int(dataTimeEnd.toTime_t()%primaryTickSecs));
    timeSeconds = timeStart.secsTo(timeEnd);

    /* Update the view properties */
    emit zoomRangeChanged(minZoomSeconds(), maxZoomSeconds());
    ensureViewTimeSpan();
    updateScrollBars();
    emit zoomSecondsChanged(viewSeconds);
    viewport()->update();
}


void EventTimelineWidget::ensureViewTimeSpan()
{
    if (viewTimeStart.isNull())
        viewTimeStart = timeStart;
    if (viewTimeEnd.isNull())
        viewTimeEnd = timeEnd;

    if (viewTimeStart < timeStart)
    {
        viewTimeEnd = viewTimeEnd.addSecs(viewTimeStart.secsTo(timeStart));
        viewTimeStart = timeStart;
    }

    if (viewTimeEnd > timeEnd)
    {
        viewTimeStart = qMax(timeStart, viewTimeStart.addSecs(viewTimeEnd.secsTo(timeEnd)));
        viewTimeEnd = timeEnd;
    }

    Q_ASSERT(viewTimeStart >= timeStart);
    Q_ASSERT(viewTimeEnd <= timeEnd);

    viewSeconds = viewTimeStart.secsTo(viewTimeEnd);
}

void EventTimelineWidget::updateRowsMap(int row)
{
    for (int n = model()->rowCount(); row < n; ++row)
    {
        EventData *data = rowData(row);
        if (!data)
            continue;

        rowsMap.insert(data, row);
    }
}

void EventTimelineWidget::addModelRows(int first, int last)
{
    if (last < 0)
        last = model()->rowCount() - 1;

    for (int i = first; i <= last; ++i)
    {
        EventData *data = rowData(i);
        if (!data)
            continue;

        LocationData *locationData;
        int pos;
        findEvent(data, true, 0, &locationData, &pos);

        locationData->events.insert(pos, data);
        rowsMap.insert(data, i);

        /* Update time span */
        if (dataTimeStart.isNull() || data->date < dataTimeStart)
            dataTimeStart = data->date;
        QDateTime ed = data->date.addSecs(qMax(data->duration,1));
        if (dataTimeEnd.isNull() || ed > dataTimeEnd)
            dataTimeEnd = ed;
    }

    updateRowsMap(last+1);
    updateTimeRange(false);
}

void EventTimelineWidget::rowsInserted(const QModelIndex &parent, int start, int end)
{
    Q_ASSERT(!parent.isValid());
    addModelRows(start, end);
    QAbstractItemView::rowsInserted(parent, start, end);
}

void EventTimelineWidget::rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end)
{
    Q_ASSERT(!parent.isValid());

    for (int i = start; i <= end; ++i)
    {
        EventData *data = rowData(i);
        if (!data)
            continue;

        ServerData *serverData;
        LocationData *locationData;
        if (!findEvent(data, false, &serverData, &locationData, 0))
            continue;

        locationData->events.removeOne(data);
        rowsMap.remove(data);

        if (locationData->events.isEmpty())
        {
            serverData->locationsMap.remove(locationData->location);
            delete locationData;

            if (serverData->locationsMap.isEmpty())
            {
                serversMap.remove(serverData->server);
                delete serverData;
            }
        }
    }

    QAbstractItemView::rowsAboutToBeRemoved(parent, start, end);
}

void EventTimelineWidget::rowsRemoved(const QModelIndex &parent, int start, int end)
{
    Q_ASSERT(!parent.isValid());

    updateRowsMap(start);
    updateTimeRange();
    viewport()->update();

    Q_ASSERT(rowsMap.size() == model()->rowCount());
}

void EventTimelineWidget::modelReset()
{
    clearData();
    addModelRows(0);
}

void EventTimelineWidget::dataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
    int firstRow = topLeft.row(), lastRow = bottomRight.row();

    for (int row = firstRow; row <= lastRow; ++row)
    {
        EventData *data = rowData(row);
        Q_ASSERT(rowsMap[data] == row);

        /* Try to find this event to handle (relatively quickly) the common case when
         * location/server do not change. */
        ServerData *server = 0;
        if (findEvent(data, false, &server, 0, 0) || !server)
            continue;

        /* Brute-force search of all locations in this server to find the old one and move it.
         * Server cannot change. */
        foreach (LocationData *location, server->locationsMap)
        {
            int pos = location->events.indexOf(data);
            if (pos < 0)
                continue;

            location->events.removeAt(pos);
            if (location->events.isEmpty())
            {
                server->locationsMap.remove(location->location);
                delete location;
            }

            break;
        }

        LocationData *location;
        int pos;
        findEvent(data, true, &server, &location, &pos);
        location->events.insert(pos, data);
    }

    updateTimeRange();
    QAbstractItemView::dataChanged(topLeft, bottomRight);
    viewport()->update();
}

QRect EventTimelineWidget::viewportItemArea() const
{
    return viewport()->rect().adjusted(leftPadding(), topPadding(), 0, 0);
}

QRect EventTimelineWidget::timeCellRect(const QDateTime &time, int duration) const
{
    Q_ASSERT(time >= timeStart);
    Q_ASSERT(time <= timeEnd);

    double range = qMax(viewSeconds, 1);

    /* Save enough room for a zero-duration item at timeEnd */
    int width = viewportItemArea().width();

    QRect r;
    r.setX(qRound((viewTimeStart.secsTo(time) / range) * width));
    r.setWidth(qMax(cellMinimum(), qRound((duration / range) * width)));
    return r;
}

void EventTimelineWidget::resizeEvent(QResizeEvent *event)
{
    updateTimeRange(false);
    QAbstractItemView::resizeEvent(event);
}

bool EventTimelineWidget::viewportEvent(QEvent *event)
{
    bool re = QAbstractItemView::viewportEvent(event);

    if (event->type() == QEvent::Polish || event->type() == QEvent::FontChange)
    {
        /* Top padding for the X-axis label text */
        QFont f = font();
        QFontMetrics fm(font());
        int height = fm.height();

        f.setBold(true);
        fm = QFontMetrics(f);
        height += fm.height();

        cachedTopPadding = height;
    }

    return re;
}

void EventTimelineWidget::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    p.eraseRect(event->rect());

    QRect r = viewport()->rect();

    QAbstractItemModel *model = this->model();
    if (!model || rowsMap.isEmpty())
        return;

    /* Draw timeline (x-axis) */
    int y = 0;

    /* Dates across the top; first one is fully qualified (space permitting) */
    p.save();
    QFont font = p.font();
    font.setBold(true);
    p.setFont(font);

    bool first = true;
    int numDays = 0;
    for (QDate date = viewTimeStart.date(), last = viewTimeEnd.date(); date <= last; date = date.addDays(1), ++numDays)
    {
        QDateTime dt = qMax(QDateTime(date), viewTimeStart);
        QRect dateRect = timeCellRect(dt, dt.secsTo(QDateTime(date.addDays(1))));
        dateRect.setHeight(r.height());
        dateRect.translate(50, 0);
        QString dateStr = date.toString(first ? tr("ddd, MMM d yyyy") : tr("ddd, MMM d"));

        /* This is very slow and could be improved dramatically with the use of QTextLayout */
        QFontMetrics fm(p.font());
        if (fm.width(dateStr)+10 > dateRect.width())
            continue;

        p.drawText(dateRect, 0, dateStr, &dateRect);
        y = qMax(y, dateRect.bottom());

        first = false;
    }
    p.restore();

    Q_ASSERT(primaryTickSecs);

    /* Draw primary ticks and text */
    QVector<QLine> lines;
    lines.reserve(qCeil(double(viewSeconds)/primaryTickSecs));

    /* Rectangle for each tick area */
    int areaWidth = viewportItemArea().width();
    QRectF tickRect(50, y, (double(primaryTickSecs) / qMax(viewSeconds,1)) * areaWidth, r.height());

    /* Round to the first tick */
    int preAreaSecs = int(viewTimeStart.toTime_t() % primaryTickSecs);
    if (preAreaSecs)
        preAreaSecs = primaryTickSecs - preAreaSecs;
    QDateTime dt = viewTimeStart.addSecs(preAreaSecs);
    tickRect.translate((double(preAreaSecs)/qMax(viewSeconds,1))*areaWidth, 0);

    for (;;)
    {
        lines.append(QLine(qRound(tickRect.x()), 1, qRound(tickRect.x()), r.bottom()));

        QString text = dt.toString(tr("h:mm"));
        QRectF textRect = tickRect.translated(qRound(tickRect.width()/-2.0), 0);

        p.drawText(textRect, Qt::AlignTop | Qt::AlignHCenter, text);

        if (textRect.right() >= r.right())
            break;

        tickRect.translate(tickRect.width(), 0);
        dt = dt.addSecs(primaryTickSecs);
    }

    y = topPadding();
    for (QVector<QLine>::Iterator it = lines.begin(); it != lines.end(); ++it)
        it->translate(0, y);

    p.save();
    p.setPen(QColor(205, 205, 205));
    p.drawLines(lines);
    p.restore();

    p.drawLine(leftPadding(), y, r.width(), y);

    /* Loop servers */
    y = topPadding();
    QRect textRect(0, 0, leftPadding(), rowHeight());
    QFont serverFont = p.font();
    serverFont.setBold(true);

    /* BUG: No sorting is done here; this shouldn't be looping the hash */
    for (QHash<DVRServer*,ServerData*>::Iterator it = serversMap.begin(); it != serversMap.end(); ++it)
    {
        ServerData *server = *it;

        textRect.moveTop(y);

        p.save();
        p.setFont(serverFont);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, server->server->displayName());
        p.restore();
        y += rowHeight();

        for (QHash<QString,LocationData*>::Iterator lit = server->locationsMap.begin();
             lit != server->locationsMap.end(); ++lit)
        {
            textRect.moveTop(y);
            p.drawText(textRect.adjusted(6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, (*lit)->location);

            QRect rowRect(leftPadding(), y, r.width(), rowHeight());
            paintRow(&p, rowRect, *lit);
            y += rowRect.height();
        }
    }

    p.drawLine(leftPadding(), topPadding(), leftPadding(), r.height());
}

void EventTimelineWidget::paintRow(QPainter *p, QRect r, LocationData *locationData)
{
    for (QList<EventData*>::Iterator it = locationData->events.begin(); it != locationData->events.end(); ++it)
    {
        EventData *data = *it;
        if (data->date.addSecs(data->duration) < viewTimeStart)
            continue;

        QRect cellRect = timeCellRect(data->date, data->duration);
        cellRect.setX(qMax(cellRect.x(), 0));
        cellRect.translate(r.x(), r.y());
        cellRect.setHeight(r.height());

        p->fillRect(cellRect, Qt::blue);
    }
}
