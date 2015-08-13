/* Copyright (c) 2015, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include "tredisdriver.h"
#include <QTcpSocket>
#include <QEventLoop>
#include <QElapsedTimer>
#include <TSystemGlobal>

#if defined(Q_OS_WIN)
#  define CRLF "\n"
#else
#  define CRLF "\r\n"
#endif

const int DEFAULT_PORT = 6379;


TRedisDriver::TRedisDriver()
    : TKvsDriver(), redis(new QTcpSocket()), buffer(), pos(0)
{
    buffer.reserve(1023);
}


TRedisDriver::~TRedisDriver()
{
    close();
    delete redis;
}


bool TRedisDriver::open(const QString &, const QString &, const QString &, const QString &host, quint16 port, const QString &)
{
    if (isOpen()) {
        return true;
    }

    QString hst = (host.isEmpty()) ? "localhost" : host;

    if (port <= 0) {
        port = DEFAULT_PORT;
    }

    tSystemDebug("Redis open host:%s  port:%d", qPrintable(hst), port);
    redis->connectToHost(hst, port);
    bool ret = waitForState(QAbstractSocket::ConnectedState, 5000);
    if (ret) {
        tSystemDebug("Redis open successfully");
    } else {
        tSystemError("Redis open failed");
    }
    return ret;
}


void TRedisDriver::close()
{
    redis->close();
}


bool TRedisDriver::isOpen() const
{
    return redis->isOpen();
}


bool TRedisDriver::readReply()
{
    if (!isOpen()) {
        tSystemError("Not open Redis session  [%s:%d]", __FILE__, __LINE__);
        return false;
    }

    if (pos > 0) {
        buffer.remove(0, pos);
        pos = 0;
    }

    QEventLoop eventLoop;
    QElapsedTimer timer;
    timer.start();

    int len = buffer.length();
    while (buffer.length() == len) {

        if (timer.elapsed() >= 2000) {
            tSystemWarn("Read timeout");
            break;
        }

        Tf::msleep(0);  // context switch
        while (eventLoop.processEvents()) {}
        buffer += redis->readAll();
    }

    //tSystemDebug("Redis reply: %s", buffer.data());
    return (buffer.length() > len);
}


bool TRedisDriver::request(const QList<QByteArray> &command, QVariantList &reply)
{
    bool ret = true;
    QByteArray str;
    bool ok = false;
    int startpos = pos;

    QByteArray cmd = toMultiBulk(command);
    //tSystemDebug("Redis command: %s", cmd.data());
    redis->write(cmd);
    redis->flush();
    clearBuffer();

    for (;;) {
        if (!readReply()) {
            clearBuffer();
            break;
        }

        switch (buffer.at(pos)) {
        case Error:
            ret = false;
            str = getLine(&ok);
            tSystemError("Redis error reply: %s", qPrintable(str));
            break;

        case SimpleString:
            str = getLine(&ok);
            tSystemDebug("Redis reply: %s", qPrintable(str));
            break;

        case Integer: {
            pos++;
            int num = getNumber(&ok);
            if (ok) {
                reply << num;
            }
            break; }

        case BulkString:
            str = parseBulkString(&ok);
            if (ok) {
                reply << str;
            }
            break;

        case Array:
            reply = parseArray(&ok);
            if (!ok) {
                reply.clear();
            }
            break;

        default:
            tSystemError("Invalid protocol: %c  [%s:%d]", buffer.at(pos), __FILE__, __LINE__);
            ret = false;
            clearBuffer();
            goto parse_done;
            break;
        }

        if (ok) {
            break;
        } else {
            pos = startpos;
        }
    }

parse_done:
    return ret;
}


QByteArray TRedisDriver::getLine(bool *ok)
{
    int idx = buffer.indexOf(CRLF, pos);
    if (idx < 0) {
        *ok = false;
        return QByteArray();
    }

    QByteArray ret = buffer.mid(pos, idx);
    pos = idx + 2;
    *ok = true;
    return ret;
}


QByteArray TRedisDriver::parseBulkString(bool *ok)
{
    QByteArray str;
    int startpos = pos;

    Q_ASSERT((int)buffer[pos] == BulkString);
    pos++;

    int len = getNumber(ok);
    if (*ok) {
        if (len < -1) {
            tSystemError("Invalid length: %d  [%s:%d]", len, __FILE__, __LINE__);
            *ok = false;
        } else if (len == -1) {
            // null string
            tSystemDebug("Null string parsed");
        } else {
            if (pos + 2 <= buffer.length()) {
                str = (len > 0) ? buffer.mid(pos, len) : QByteArray("");
                pos += len + 2;
            } else {
                *ok = false;
            }
        }
    }

    if (! *ok) {
        pos = startpos;
    }
    return str;
}


QVariantList TRedisDriver::parseArray(bool *ok)
{
    QVariantList lst;
    int startpos = pos;
    *ok = false;

    Q_ASSERT((int)buffer[pos] == Array);
    pos++;

    int count = getNumber(ok);
    while (*ok) {
        switch (buffer[pos]) {
        case BulkString: {
            auto str = parseBulkString(ok);
            if (*ok) {
                lst << str;
            }
            break; }

        case Integer: {
            pos++;
            int num = getNumber(ok);
            if (*ok) {
                lst << num;
            }
            break; }

        case Array: {
            auto var = parseArray(ok);
            if (*ok) {
                lst << QVariant(var);
            }
            break; }

        default:
            tSystemError("Bad logic  [%s:%d]", __FILE__, __LINE__);
            *ok = false;
            break;
        }

        if (lst.count() >= count) {
            break;
        }
    }

    if (! *ok) {
        pos = startpos;
    }
    return lst;
}


int TRedisDriver::getNumber(bool *ok)
{
    int num = 0;

    int idx = buffer.indexOf(CRLF, pos);
    if (idx < 0) {
        *ok = false;
        return num;
    }

    int c = 1;
    char d = buffer[pos++];

    if (d == '-') {
        c = -1;
        d = buffer[pos++];
    }

    while (d >= '0' && d <= '9') {
        num *= 10;
        num += d - '0';
        d = buffer[pos++];
    }

    pos = idx + 2;
    *ok = true;
    return num * c;
}


void TRedisDriver::clearBuffer()
{
    buffer.resize(0);
    pos = 0;
}


QByteArray TRedisDriver::toBulk(const QByteArray &data)
{
    QByteArray bulk("$");
    bulk += QByteArray::number(data.length());
    bulk += CRLF;
    bulk += data;
    bulk += CRLF;
    return bulk;
}


QByteArray TRedisDriver::toMultiBulk(const QList<QByteArray> &data)
{
    QByteArray mbulk;
    mbulk += "*";
    mbulk += QByteArray::number(data.count());
    mbulk += CRLF;
    for (auto &d : data) {
        mbulk += toBulk(d);
    }
    return mbulk;
}


bool TRedisDriver::waitForState(int state, int msecs)
{
    QEventLoop eventLoop;
    QElapsedTimer timer;
    timer.start();

    while (redis->state() != state) {
        if (timer.elapsed() >= msecs) {
            tSystemWarn("waitForState timeout.  current state:%d  timeout:%d", redis->state(), msecs);
            return false;
        }

        Tf::msleep(1);
        while (eventLoop.processEvents()) {}
    }
    return true;
}
