/*  Copyright (C) 2020 NANDO authors
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 */

#include "writer.h"
#include "err.h"
#include <QDebug>

#define READ_ACK_TIMEOUT 5000

Writer::Writer()
{
}

Writer::~Writer()
{
}

void Writer::init(SerialPort *serialPort, QVector<uint8_t> *buf,
    quint64 addr, quint64 len, uint32_t pageSize, bool skipBB, bool incSpare,
    bool enableHwEcc, uint8_t startCmd, uint8_t dataCmd, uint8_t endCmd)
{
    this->serialPort = serialPort;
    this->buf = buf;
    this->addr = addr;
    this->len = len;
    this->pageSize = pageSize;
    this->skipBB = skipBB;
    this->incSpare = incSpare;
    this->enableHwEcc = enableHwEcc;
    this->startCmd = startCmd;
    this->dataCmd = dataCmd;
    this->endCmd = endCmd;
    bytesWritten = 0;
    bytesAcked = 0;
    offset = 0;
}

int Writer::write(char *data, uint32_t dataLen)
{
    qint64 ret;

    ret = serialPort->write(reinterpret_cast<const char *>(data), dataLen);
    if (ret < 0)
        return -1;
    else if (static_cast<uint32_t>(ret) < dataLen)
    {
        logErr(QString("数据被部分写入，返回%1，应为%2")
            .arg(ret).arg(dataLen));
        return -1;
    }

    return 0;
}

int Writer::read(char *data, uint32_t dataLen)
{
    std::function<void(int)> cb = std::bind(&Writer::readCb, this,
        std::placeholders::_1);

    if (serialPort->asyncRead(data, dataLen, cb, READ_ACK_TIMEOUT)
        < 0)
    {
        return -1;
    }

    return 0;
}

int Writer::handleWriteAck(RespHeader *header, uint32_t len)
{
    int size = sizeof(RespWriteAck);

    if (len < static_cast<uint32_t>(size))
    {
        return 0;
    }

    bytesAcked = (reinterpret_cast<RespWriteAck *>(header))->ackBytes;

    if (bytesAcked != bytesWritten)
    {
        logErr(QString("收到错误确认 %1, 应为 %2 ").arg(bytesAcked)
            .arg(bytesWritten));
        return -1;
    }

    emit progress(bytesAcked);

    return size;
}

int Writer::handleBadBlock(RespHeader *header, uint32_t len, bool isSkipped)
{
    int size = sizeof(RespBadBlock);
    RespBadBlock *badBlock = reinterpret_cast<RespBadBlock *>(header);
    QString message = isSkipped ? "跳过位于 0x%1 大小为 0x%2 的坏块" :
        "坏块位于 0x%1 大小为 0x%2";

    if (len < static_cast<uint32_t>(size))
        return 0;

    logInfo(message.arg(badBlock->addr, 8, 16, QLatin1Char('0'))
        .arg(badBlock->size, 8, 16, QLatin1Char('0')));

    return size;
}

int Writer::handleError(RespHeader *header, uint32_t len)
{
    RespError *err = reinterpret_cast<RespError *>(header);
    int size = sizeof(RespError);

    if (len < static_cast<uint32_t>(size))
        return 0;

    logErr(QString("编程器返回错误: (%1) %2").arg(err->errCode)
        .arg(errCode2str(-err->errCode)));

    return -1;
}

int Writer::handleStatus(char *pbuf, uint32_t len)
{
    RespHeader *header = reinterpret_cast<RespHeader *>(pbuf);
    uint8_t status = header->info;

    switch (status)
    {
    case STATUS_OK:
        return sizeof(RespHeader);
    case STATUS_ERROR:
        return handleError(header, len);
    case STATUS_BB:
        return handleBadBlock(header, len, false);
    case STATUS_BB_SKIP:
        return handleBadBlock(header, len, true);
    case STATUS_WRITE_ACK:
        return handleWriteAck(header, len);
    }

    logErr(QString("编程器返回错误的响应代码 %1").arg(status));
    return -1;
}

int Writer::handlePacket(char *pbuf, uint32_t len)
{
    RespHeader *header = reinterpret_cast<RespHeader *>(pbuf);

    if (len < sizeof(RespHeader))
        return 0;

    if (header->code != RESP_STATUS)
    {
        logErr(QString("编程器返回错误的响应代码: %1")
            .arg(header->code));
        return -1;
    }

    return handleStatus(pbuf, len);
}

int Writer::handlePackets(char *pbuf, uint32_t len)
{
    int ret;
    uint32_t offset = 0;

    do
    {
        if ((ret = handlePacket(pbuf + offset, len - offset)) < 0)
            return -1;

        if (ret)
            offset += static_cast<uint32_t>(ret);
        else
        {
            memmove(pbuf, pbuf + offset, len - offset);
            break;
        }
    }
    while (offset < len);

    return static_cast<int>(len - offset);
}

void Writer::readCb(int size)
{
    if (size < 0)
        goto Error;

    size += offset;

    if ((offset = handlePackets(pbuf, static_cast<uint32_t>(size))) < 0)
        goto Error;

    if (offset)
    {
        if (read(pbuf + offset, bufSize - offset) < 0)
            goto Error;
        return;
    }

    if (cmd == startCmd)
    {
        if (writeData())
            goto Error;
    }
    else if (cmd == dataCmd)
    {
        if (len)
        {
            if (writeData())
                goto Error;
        }
        else if (writeEnd())
            goto Error;
    }
    else if (cmd == endCmd)
        emit result(0);

    return;

Error:
    emit result(-1);
}

int Writer::writeStart()
{
    WriteStartCmd writeStartCmd;

    writeStartCmd.cmd.code = startCmd;
    writeStartCmd.addr = addr;
    writeStartCmd.len = len;
    writeStartCmd.flags.skipBB = skipBB;
    writeStartCmd.flags.incSpare = incSpare;
    writeStartCmd.flags.enableHwEcc = enableHwEcc;
    cmd = startCmd;

    if (write(reinterpret_cast<char *>(&writeStartCmd),
        sizeof(WriteStartCmd)))
    {
        return -1;
    }

    if (read(pbuf, bufSize))
        return -1;

    return 0;
}

int Writer::writeData()
{
    WriteDataCmd *writeDataCmd = reinterpret_cast<WriteDataCmd *>(pbuf);
    uint32_t dataLen, dataLenMax, headerLen, pageLim, bufWriten = 0;

    writeDataCmd->cmd.code = dataCmd;
    headerLen = sizeof(WriteDataCmd);
    dataLenMax = bufSize - headerLen;
    cmd = dataCmd;

    while (len)
    {
        dataLen = len < dataLenMax ? len : dataLenMax;

        pageLim = bytesAcked + pageSize;
        if (dataLen + bytesWritten > pageLim)
            dataLen = pageLim - bytesWritten;

        writeDataCmd->len = static_cast<uint8_t>(dataLen);
        memcpy(pbuf + headerLen, buf->constData() + bufWriten, dataLen);
        if (write(pbuf, headerLen + dataLen))
            return -1;

        bufWriten += dataLen;
        bytesWritten += dataLen;
        len -= dataLen;

        if (!len || bytesWritten == pageLim)
            break;
    }

    if (read(pbuf, bufSize))
        return -1;

    return 0;
}

int Writer::writeEnd()
{
    WriteEndCmd writeEndCmd;

    writeEndCmd.cmd.code = endCmd;
    cmd = endCmd;

    if (write(reinterpret_cast<char *>(&writeEndCmd), sizeof(WriteEndCmd)))
        return -1;

    if (read(pbuf, bufSize))
        return -1;

    return 0;
}

void Writer::start()
{
    if (writeStart())
        emit result(-1);
}

void Writer::logErr(const QString& msg)
{
    emit log(QtCriticalMsg, msg);
}

void Writer::logInfo(const QString& msg)
{
    emit log(QtInfoMsg, msg);
}

