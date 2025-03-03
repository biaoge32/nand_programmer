/*  Copyright (C) 2020 NANDO authors
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 */

#ifndef SPI_NOR_DB_TABLE_MODEL_H
#define SPI_NOR_DB_TABLE_MODEL_H

#include "spi_nor_db.h"
#include <QAbstractTableModel>

class SpiNorDbTableModel : public QAbstractTableModel
{
    Q_OBJECT

    SpiNorDb *chipDb;

public:
    explicit SpiNorDbTableModel(SpiNorDb *chipDb, QObject *parent = nullptr);
    int rowCount(const QModelIndex & /*parent*/) const override;
    int columnCount(const QModelIndex & /*parent*/) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role)
        const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
        int role = Qt::EditRole) override;
    void addRow();
    void delRow(int index);
    void commit();
    void reset();
};

#endif // SPI_NOR_DB_TABLE_MODEL_H
